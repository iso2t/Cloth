// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::Result;
use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Component, Path, PathBuf};

#[derive(Deserialize)]
struct Manifest {
    project: Package,
    #[serde(default)]
    modules: BTreeMap<String, PathBuf>,
    #[serde(default)]
    dependencies: BTreeMap<String, Dependency>,
    output: Option<Output>,
}

#[derive(Deserialize)]
struct Package {
    name: String,
    #[serde(default)]
    kind: Kind,
    module: Option<String>,
    entry: Option<PathBuf>,
    #[serde(rename = "entryPoint")]
    entry_point: Option<String>,
}

#[derive(Clone, Copy, Default, PartialEq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    #[default]
    Executable,
    Library,
}

#[derive(Deserialize)]
struct Output {
    directory: PathBuf,
}

#[derive(Deserialize)]
#[serde(untagged)]
enum Dependency {
    Source(PathBuf),
    Project { project: PathBuf, prebuilt: bool },
}

pub struct Project {
    pub name: String,
    pub module: String,
    pub kind: Kind,
    pub entry: PathBuf,
    pub output: PathBuf,
    pub dependencies: BTreeSet<usize>,
    pub prebuilt: bool,
}

impl Project {
    pub fn artifact(&self, target: &str) -> PathBuf {
        let name = match self.kind {
            Kind::Library => format!("{}.o", self.name),
            Kind::Executable if target.contains("windows") || target.contains("mingw") => {
                format!("{}.exe", self.name)
            }
            Kind::Executable => self.name.clone(),
        };
        self.output.join(target).join(name)
    }
}

fn filename(name: &str) -> bool {
    matches!(Path::new(name).components().collect::<Vec<_>>().as_slice(), [Component::Normal(part)] if *part == name)
        && !name.contains(['/', '\\', ':'])
}

impl Manifest {
    fn entry(&self, directory: &Path) -> Result<PathBuf> {
        match (&self.project.entry, &self.project.entry_point) {
            (Some(path), None) => Ok(directory.join(path)),
            (None, Some(entry)) => {
                let mut parts: Vec<_> = entry.split('.').collect();
                if self.project.kind == Kind::Executable && parts.pop() != Some("main") {
                    return Err(format!(
                        "entryPoint '{entry}' must end in .main for an executable"
                    )
                    .into());
                }
                if parts.len() < 2 || !parts.iter().all(|part| filename(part)) {
                    return Err(format!("invalid module entryPoint '{entry}'").into());
                }
                let root = self
                    .modules
                    .get(parts[0])
                    .ok_or_else(|| format!("unknown module root '{}'", parts[0]))?;
                let mut path = directory.join(root);
                path.extend(&parts[1..]);
                Ok(path.with_extension("co"))
            }
            _ => Err("set exactly one of project.entry and project.entryPoint".into()),
        }
    }
}

#[derive(Default)]
pub struct Graph {
    pub projects: Vec<Project>,
    loaded: BTreeMap<PathBuf, usize>,
    visiting: BTreeSet<PathBuf>,
    modules: BTreeMap<String, PathBuf>,
}

impl Graph {
    pub fn load(&mut self, path: &Path) -> Result<usize> {
        self.visit(path, false)
    }

    pub fn check_destination(&self, artifact: &Path, target: &str) -> Result<()> {
        if !artifact.exists() {
            return Ok(());
        }
        let existing = dunce::canonicalize(artifact)?;
        let same_file = |path: &Path| dunce::canonicalize(path).is_ok_and(|path| path == existing);
        if self.loaded.contains_key(&existing)
            || self.projects.iter().any(|project| {
                same_file(&project.entry)
                    || (project.prebuilt && same_file(&project.artifact(target)))
            })
        {
            return Err(format!(
                "output would overwrite a project input at {}",
                artifact.display()
            )
            .into());
        }
        Ok(())
    }

    fn visit(&mut self, path: &Path, prebuilt: bool) -> Result<usize> {
        let path = if path.is_dir() {
            path.join("build.toml")
        } else {
            path.to_owned()
        };
        let path =
            dunce::canonicalize(&path).map_err(|error| format!("{}: {error}", path.display()))?;
        if let Some(&index) = self.loaded.get(&path) {
            self.projects[index].prebuilt &= prebuilt;
            return Ok(index);
        }
        if !self.visiting.insert(path.clone()) {
            return Err(format!("cyclic project dependency at {}", path.display()).into());
        }
        let text = fs::read_to_string(&path)?;
        let manifest: Manifest =
            toml::from_str(&text).map_err(|error| format!("{}: {error}", path.display()))?;
        let directory = path
            .parent()
            .ok_or("a manifest requires a parent directory")?;
        let entry = manifest.entry(directory)?;
        let package = manifest.project;
        if !filename(&package.name) {
            return Err(format!("invalid project name '{}'", package.name).into());
        }
        let module = package.module.unwrap_or_else(|| package.name.clone());
        if package.kind == Kind::Library {
            if let Some(previous) = self.modules.insert(module.clone(), path.clone()) {
                return Err(format!(
                    "module '{module}' is supplied by {} and {}",
                    previous.display(),
                    path.display()
                )
                .into());
            }
        } else if prebuilt {
            return Err(format!("{} is an executable dependency", path.display()).into());
        }
        let mut dependencies = BTreeSet::new();
        for (name, dependency) in manifest.dependencies {
            let (location, installed) = match dependency {
                Dependency::Source(path) => (path, false),
                Dependency::Project { project, prebuilt } => (project, prebuilt),
            };
            let index = self.visit(&directory.join(location), installed)?;
            let library = &self.projects[index];
            if library.kind != Kind::Library || library.module != name {
                return Err(format!(
                    "dependency '{name}' must declare a library with that module name"
                )
                .into());
            }
            dependencies.extend(&library.dependencies);
            dependencies.insert(index);
        }
        let index = self.projects.len();
        self.projects.push(Project {
            name: package.name,
            module,
            kind: package.kind,
            entry,
            output: directory.join(
                manifest
                    .output
                    .map_or_else(|| PathBuf::from("build"), |output| output.directory),
            ),
            dependencies,
            prebuilt,
        });
        self.visiting.remove(&path);
        self.loaded.insert(path, index);
        Ok(index)
    }
}
