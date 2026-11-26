// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Shuttle/README.rst

use crate::Result;
use kdl::{KdlDocument, KdlEntry, KdlNode};
use std::collections::{BTreeMap, BTreeSet};
use std::error::Error;
use std::fmt::Display;
use std::fs;
use std::path::{Path, PathBuf};
use unicode_normalization::UnicodeNormalization;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Kind {
    Executable,
    Library,
}

// Module import name and the manifest location of its uses declaration.
pub type Uses = BTreeMap<String, String>;

#[derive(Clone)]
pub struct Target {
    pub kind: Kind,
    pub entry: Option<PathBuf>,
    pub module: Option<String>,
    pub uses: Uses,
}

pub struct Dependency {
    pub path: PathBuf,
    pub prebuilt: bool,
}

pub struct Manifest {
    pub name: String,
    pub default_target: Option<String>,
    pub targets: BTreeMap<String, Target>,
    pub includes: BTreeSet<PathBuf>,
    pub dependencies: BTreeMap<String, Dependency>,
    pub output: PathBuf,
}

pub struct Module {
    pub name: String,
    pub location: String,
    pub api: Option<PathBuf>,
    pub uses: Uses,
}

struct Source<'a> {
    path: &'a Path,
    text: &'a str,
    line_starts: Vec<usize>,
}

impl Source<'_> {
    fn location(&self, offset: usize) -> String {
        let line = self.line_starts.partition_point(|&start| start <= offset);
        let column = self.text[self.line_starts[line - 1]..offset]
            .chars()
            .count()
            + 1;
        format!("{}:{line}:{column}", self.path.display())
    }

    fn error(&self, offset: usize, message: impl Display) -> Box<dyn Error> {
        format!("{}: {message}", self.location(offset)).into()
    }

    // Every manifest node has one string argument and a fixed set of properties.
    fn argument<'a>(
        &self,
        node: &'a KdlNode,
        properties: &[&str],
        children: bool,
    ) -> Result<&'a str> {
        let mut argument = None;
        let mut seen = BTreeSet::new();
        for entry in node.entries() {
            if let Some(key) = entry.name() {
                let key = key.value();
                if !properties.contains(&key) {
                    return Err(self.error(
                        entry.span().offset(),
                        format!("unknown property '{key}' on '{}'", node.name().value()),
                    ));
                }
                if !seen.insert(key) {
                    return Err(
                        self.error(entry.span().offset(), format!("duplicate property '{key}'"))
                    );
                }
            } else if argument.is_some() {
                return Err(self.error(entry.span().offset(), "expected one argument"));
            } else {
                argument = Some(self.string(entry)?);
            }
        }
        if !children && node.children().is_some() {
            return Err(self.error(
                node.span().offset(),
                format!("'{}' cannot contain child nodes", node.name().value()),
            ));
        }
        argument.ok_or_else(|| {
            self.error(
                node.span().offset(),
                format!("'{}' requires a string argument", node.name().value()),
            )
        })
    }

    fn string<'a>(&self, entry: &'a KdlEntry) -> Result<&'a str> {
        entry
            .value()
            .as_string()
            .filter(|s| !s.is_empty())
            .ok_or_else(|| self.error(entry.span().offset(), "expected a nonempty string"))
    }

    fn property<'a>(&self, node: &'a KdlNode, key: &str) -> Result<Option<&'a str>> {
        node.entry(key).map(|entry| self.string(entry)).transpose()
    }

    fn uses(&self, node: &KdlNode) -> Result<Uses> {
        let mut uses = Uses::new();
        for child in node.iter_children() {
            if child.name().value() != "uses" {
                return Err(self.error(
                    child.span().offset(),
                    format!(
                        "unknown node '{}' in '{}'",
                        child.name().value(),
                        node.name().value()
                    ),
                ));
            }
            let name = self.argument(child, &[], false)?;
            if uses
                .insert(
                    self.identifier(child, name)?,
                    self.location(child.span().offset()),
                )
                .is_some()
            {
                return Err(self.error(child.span().offset(), format!("duplicate use of '{name}'")));
            }
        }
        Ok(uses)
    }

    fn identifier(&self, node: &KdlNode, name: &str) -> Result<String> {
        let mut chars = name.chars();
        if !chars
            .next()
            .is_some_and(|c| c == '_' || unicode_ident::is_xid_start(c))
            || !chars.all(unicode_ident::is_xid_continue)
        {
            return Err(self.error(
                node.span().offset(),
                format!("module name '{name}' must be a Cloth identifier"),
            ));
        }
        Ok(name.nfc().collect())
    }
}

fn read<T>(
    path: &Path,
    root: &str,
    decode: impl FnOnce(&Source, &KdlNode) -> Result<T>,
) -> Result<T> {
    let text = fs::read_to_string(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut line_starts = vec![0];
    for (index, character) in text.char_indices() {
        match character {
            '\r' if text.as_bytes().get(index + 1) == Some(&b'\n') => {}
            '\r' | '\n' | '\u{0085}' | '\u{000b}' | '\u{000c}' | '\u{2028}' | '\u{2029}' => {
                line_starts.push(index + character.len_utf8());
            }
            _ => {}
        }
    }
    let source = Source {
        path,
        text: &text,
        line_starts,
    };
    let doc = KdlDocument::parse_v2(&text).map_err(|error| {
        if let Some(first) = error.diagnostics.first() {
            source.error(
                first.span.offset(),
                first.message.as_deref().unwrap_or("invalid KDL"),
            )
        } else {
            source.error(0, error)
        }
    })?;
    let node = match doc.nodes() {
        [node] if node.name().value() == root => node,
        nodes => {
            let offset = nodes
                .iter()
                .find(|n| n.name().value() != root)
                .or(nodes.get(1))
                .map_or(0, |n| n.span().offset());
            return Err(source.error(offset, format!("expected one '{root}' node")));
        }
    };
    decode(&source, node)
}

fn filename(name: &str) -> bool {
    Path::new(name).file_name().is_some_and(|part| part == name)
        && !name.contains(['/', '\\', ':', '\0'])
}

impl Module {
    pub fn read(path: &Path) -> Result<Self> {
        read(path, "module", |source, node| {
            let name = source.argument(node, &["api"], true)?;
            Ok(Self {
                name: source.identifier(node, name)?,
                location: source.location(node.span().offset()),
                api: source.property(node, "api")?.map(PathBuf::from),
                uses: source.uses(node)?,
            })
        })
    }
}

impl Manifest {
    pub fn read(path: &Path) -> Result<Self> {
        read(path, "project", |source, node| {
            let name = source.argument(node, &["version"], true)?;
            if !filename(name) {
                return Err(source.error(
                    node.span().offset(),
                    format!("invalid project name '{name}'"),
                ));
            }
            source.property(node, "version")?;
            let mut manifest = Self {
                name: name.to_owned(),
                default_target: None,
                targets: BTreeMap::new(),
                includes: BTreeSet::new(),
                dependencies: BTreeMap::new(),
                output: PathBuf::new(),
            };
            for child in node.iter_children() {
                let offset = child.span().offset();
                match child.name().value() {
                    "include" => {
                        let dir = source.argument(child, &[], false)?;
                        if !filename(dir) {
                            return Err(
                                source.error(offset, "include requires a direct child directory")
                            );
                        }
                        if !manifest.includes.insert(dir.into()) {
                            return Err(source.error(offset, format!("duplicate include '{dir}'")));
                        }
                    }
                    "output" => {
                        let dir = source.argument(child, &[], false)?;
                        if !manifest.output.as_os_str().is_empty() {
                            return Err(source.error(offset, "duplicate output directory"));
                        }
                        manifest.output = dir.into();
                    }
                    "target" => manifest.add_target(source, child)?,
                    "dependency" => {
                        let name = source.argument(child, &["path", "prebuilt"], false)?;
                        let path = source
                            .property(child, "path")?
                            .ok_or_else(|| source.error(offset, "dependency requires a path"))?;
                        let prebuilt = child
                            .entry("prebuilt")
                            .map(|entry| {
                                entry.value().as_bool().ok_or_else(|| {
                                    source.error(
                                        entry.span().offset(),
                                        "prebuilt requires #true or #false",
                                    )
                                })
                            })
                            .transpose()?
                            .unwrap_or(false);
                        if manifest
                            .dependencies
                            .insert(
                                source.identifier(child, name)?,
                                Dependency {
                                    path: path.into(),
                                    prebuilt,
                                },
                            )
                            .is_some()
                        {
                            return Err(
                                source.error(offset, format!("duplicate dependency '{name}'"))
                            );
                        }
                    }
                    other => {
                        return Err(source.error(offset, format!("unknown project node '{other}'")));
                    }
                }
            }
            if manifest.targets.is_empty() {
                return Err(
                    source.error(node.span().offset(), "project requires at least one target")
                );
            }
            if manifest.output.as_os_str().is_empty() {
                manifest.output = "build".into();
            }
            Ok(manifest)
        })
    }

    fn add_target(&mut self, source: &Source, node: &KdlNode) -> Result<()> {
        let offset = node.span().offset();
        let kind = match source.argument(node, &["name", "entry", "module"], true)? {
            "executable" => Kind::Executable,
            "library" => Kind::Library,
            other => return Err(source.error(offset, format!("unknown target kind '{other}'"))),
        };
        let name = source.property(node, "name")?;
        let target_name = name.unwrap_or(&self.name).to_owned();
        if !filename(&target_name) {
            return Err(source.error(offset, format!("invalid target name '{target_name}'")));
        }
        let entry = source.property(node, "entry")?.map(PathBuf::from);
        let module = source.property(node, "module")?;
        if kind == Kind::Executable && (entry.is_none() || module.is_some()) {
            return Err(source.error(
                offset,
                "executable target requires an entry and accepts no module property",
            ));
        }
        if entry.is_none() && module.is_none() {
            return Err(source.error(offset, "library target requires an entry or module"));
        }
        let module = (kind == Kind::Library)
            .then(|| source.identifier(node, module.unwrap_or(&self.name)))
            .transpose()?;
        if name.is_none() {
            self.default_target = Some(target_name.clone());
        }
        let target = Target {
            kind,
            entry,
            module,
            uses: source.uses(node)?,
        };
        if self.targets.insert(target_name.clone(), target).is_some() {
            return Err(source.error(offset, format!("duplicate target name '{target_name}'")));
        }
        Ok(())
    }
}
