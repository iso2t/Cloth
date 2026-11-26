// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Shuttle/README.rst

use crate::{HELP, Result, manifest, project};
use kdl::KdlDocument;
use std::ffi::OsString;
use std::fs::{self, File};
use std::io::{ErrorKind, Write};
use std::path::{Path, PathBuf};

const MAIN: &str =
    "using cloth.Console : println;\n\npub fn main() {\n    println(\"Hello, world!\");\n}\n";
const API: &str = "pub fn<int> answer() = 42;\n";

// Compiler/src/token/token.cpp defines the reserved keywords. The lexer also
// treats a lone underscore as a discard token.
const RESERVED: &[&str] = &[
    "_",
    "bool",
    "break",
    "char",
    "class",
    "const",
    "continue",
    "def",
    "default",
    "delete",
    "do",
    "else",
    "enum",
    "false",
    "final",
    "float",
    "fn",
    "for",
    "if",
    "init",
    "int",
    "interface",
    "interop",
    "is",
    "isize",
    "mod",
    "mt",
    "new",
    "null",
    "operator",
    "opt",
    "override",
    "pub",
    "return",
    "self",
    "state",
    "string",
    "struct",
    "true",
    "typedef",
    "typeinfo",
    "unsigned",
    "using",
    "usize",
    "virtual",
    "void",
    "while",
];

pub fn run(args: impl Iterator<Item = OsString>) -> Result<()> {
    let mut module = false;
    let mut destination = None;
    for arg in args {
        match arg.to_str() {
            Some("-h" | "--help") => {
                println!("{HELP}");
                return Ok(());
            }
            Some("--module") if !module => module = true,
            Some(flag) if flag.starts_with('-') => {
                return Err(format!("unexpected init option '{flag}'").into());
            }
            _ => {
                if destination.replace(arg).is_some() {
                    return Err("shuttle init accepts one project path or module name".into());
                }
            }
        }
    }
    if module {
        let name = destination
            .as_deref()
            .ok_or("--module requires a module name")?;
        create_module(name.to_str().ok_or("module names must be UTF-8")?)
    } else {
        create_project(Path::new(&destination.unwrap_or_else(|| ".".into())))
    }
}

// Roll back only paths created by this invocation, leaving existing directories
// and any files added by another process in place.
#[derive(Default)]
struct Created {
    files: Vec<PathBuf>,
    directories: Vec<PathBuf>,
}

impl Created {
    fn directory(&mut self, path: &Path) -> Result<()> {
        fs::create_dir(path).map_err(|e| format!("creating {}: {e}", path.display()))?;
        self.directories.push(path.to_owned());
        Ok(())
    }

    fn ensure_directory(&mut self, path: &Path) -> Result<()> {
        if path.is_dir() {
            return Ok(());
        }
        if let Some(parent) = path.parent() {
            self.ensure_directory(parent)?;
        }
        self.directory(path)
    }

    fn file(&mut self, path: &Path) -> Result<File> {
        let file =
            File::create_new(path).map_err(|e| format!("creating {}: {e}", path.display()))?;
        self.files.push(path.to_owned());
        Ok(file)
    }

    fn write(&mut self, path: &Path, contents: &str) -> Result<()> {
        self.file(path)?.write_all(contents.as_bytes())?;
        Ok(())
    }

    fn commit(mut self) {
        self.files.clear();
        self.directories.clear();
    }
}

impl Drop for Created {
    fn drop(&mut self) {
        for path in self.files.iter().rev() {
            let _ = fs::remove_file(path);
        }
        for path in self.directories.iter().rev() {
            let _ = fs::remove_dir(path);
        }
    }
}

fn create_project(path: &Path) -> Result<()> {
    let mut created = Created::default();
    let directory = std::path::absolute(path)?;
    created.ensure_directory(&directory)?;
    let directory = dunce::canonicalize(directory)?;
    let name = directory
        .file_name()
        .and_then(|n| n.to_str())
        .filter(|n| manifest::filename(n))
        .ok_or_else(|| {
            format!(
                "{} needs a valid project directory name",
                directory.display()
            )
        })?;
    let manifest = directory.join("build.k");
    if manifest.try_exists()? {
        return Err(format!("{} already exists", manifest.display()).into());
    }
    let src = directory.join("src");
    match fs::symlink_metadata(&src) {
        Ok(entry) if entry.is_dir() => {}
        Ok(_) => return Err(format!("{} must be a project directory", src.display()).into()),
        Err(e) if e.kind() == ErrorKind::NotFound => created.directory(&src)?,
        Err(e) => return Err(e.into()),
    }
    created.write(&src.join("main.co"), MAIN)?;
    created.write(&manifest, &format!(
        "project {name:?} version=\"0.1.0\" {{\n    target \"executable\" entry=\"src/main.co\"\n}}\n"
    ))?;
    created.commit();
    println!("Created project '{name}' in {}", directory.display());
    Ok(())
}

fn with_include(text: &str, name: &str) -> Result<String> {
    let doc = KdlDocument::parse_v2(text)?;
    let first = doc
        .get("project")
        .and_then(|p| p.iter_children().next())
        .ok_or("project requires at least one target")?;
    // Insert before the first child's leading comments. Leave the original
    // manifest bytes intact, including inline nodes and the file's line endings.
    let leading = first.format().map_or("", |f| f.leading.as_str());
    let offset = first.span().offset() - leading.len();
    let addition = if let Some(eol) = ["\r\n", "\n", "\r"]
        .into_iter()
        .find(|eol| leading.starts_with(eol))
    {
        let indent = leading.rsplit(['\r', '\n']).next().unwrap_or("");
        let indent = if indent.trim().is_empty() {
            indent
        } else {
            "    "
        };
        format!("{eol}{indent}include \"{name}\"")
    } else {
        format!(" include \"{name}\";")
    };
    let mut updated = text.to_owned();
    updated.insert_str(offset, &addition);
    Ok(updated)
}

fn create_module(name: &str) -> Result<()> {
    let key = manifest::identifier(name)
        .filter(|n| !RESERVED.contains(&n.as_str()))
        .ok_or_else(|| format!("module name '{name}' must be a Cloth identifier"))?;
    let manifest = project::enclosing_manifest(Path::new("."))?;
    let root = manifest
        .parent()
        .ok_or("a manifest requires a parent directory")?;
    let mut created = Created::default();
    // An exclusive sibling file serializes Shuttle editors and lets us publish
    // the manifest in one rename after the module sources are ready.
    let staged = root.join(".build.k.shuttle-init");
    let mut pending = created.file(&staged)?;
    let original =
        fs::read_to_string(&manifest).map_err(|e| format!("{}: {e}", manifest.display()))?;
    project::check_new_module(&manifest, &key)?;
    let updated = with_include(&original, name)?;
    let permissions = fs::metadata(&manifest)?.permissions();
    if permissions.readonly() {
        return Err(format!("{} is read-only", manifest.display()).into());
    }
    pending.write_all(updated.as_bytes())?;
    pending.set_permissions(permissions)?;
    drop(pending);

    let directory = root.join(name);
    created.directory(&directory)?;
    let src = directory.join("src");
    created.directory(&src)?;
    created.write(&src.join("api.co"), API)?;
    created.write(
        &directory.join("module.k"),
        &format!("module \"{name}\" api=\"src/api.co\"\n"),
    )?;
    if fs::read_to_string(&manifest)? != original {
        return Err(format!(
            "{} changed during initialization; retry",
            manifest.display()
        )
        .into());
    }
    fs::rename(&staged, &manifest).map_err(|e| format!("updating {}: {e}", manifest.display()))?;
    created.commit();
    println!("Created module '{name}' in {}", directory.display());
    println!("Add uses \"{name}\" to the target or module that imports it.");
    Ok(())
}
