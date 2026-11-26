// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::Result;
use crate::manifest::{Kind, Manifest, Module, Target, Uses};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Component, Path, PathBuf};
use std::rc::Rc;

pub struct Project {
    pub name: String,
    pub module: String,
    pub kind: Kind,
    pub entry: PathBuf,
    pub output: PathBuf,
    pub dependencies: BTreeSet<usize>,
    pub prebuilt: bool,
}

impl Kind {
    fn artifact_name(self, name: &str, target: &str) -> String {
        match self {
            Self::Library => format!("{name}.o"),
            Self::Executable if target.contains("windows") || target.contains("mingw") => {
                format!("{name}.exe")
            }
            Self::Executable => name.to_owned(),
        }
    }
}

impl Project {
    pub fn artifact(&self, target: &str) -> PathBuf {
        self.output
            .join(target)
            .join(self.kind.artifact_name(&self.name, target))
    }
}

fn canonical(path: &Path) -> Result<PathBuf> {
    dunce::canonicalize(path).map_err(|error| format!("{}: {error}", path.display()).into())
}

fn source_file(directory: &Path, relative: &Path) -> Result<PathBuf> {
    if relative
        .components()
        .any(|c| matches!(c, Component::Prefix(_) | Component::RootDir))
    {
        return Err(format!(
            "entry '{}' must be relative to {}",
            relative.display(),
            directory.display()
        )
        .into());
    }
    let path = canonical(&directory.join(relative))?;
    if !path.is_file() {
        return Err(format!("entry '{}' must be a file", path.display()).into());
    }
    Ok(path)
}

struct IncludedModule {
    manifest: PathBuf,
    directory: PathBuf,
    root: PathBuf,
    definition: Module,
}

struct Package {
    path: PathBuf,
    directory: PathBuf,
    root: Option<PathBuf>,
    manifest: Manifest,
    modules: BTreeMap<String, IncludedModule>,
    // An external project can consume only a library that has a build target.
    libraries: BTreeMap<String, String>,
}

#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
enum Unit {
    Target(String),
    LocalModule(String),
}

impl Unit {
    fn name(&self) -> &str {
        match self {
            Self::Target(name) | Self::LocalModule(name) => name,
        }
    }
}

impl Package {
    fn read(path: &Path) -> Result<Self> {
        let directory = path
            .parent()
            .ok_or("a manifest requires a parent directory")?
            .to_owned();
        let manifest = Manifest::read(path)?;
        let root = directory.join("src");
        let root = root.is_dir().then(|| canonical(&root)).transpose()?;
        let mut modules: BTreeMap<String, IncludedModule> = BTreeMap::new();
        for include in &manifest.includes {
            let directory = directory.join(include);
            let file = canonical(&directory.join("module.k"))?;
            let definition = Module::read(&file)?;
            let name = definition.name.clone();
            if let Some(previous) = modules.get(&name) {
                return Err(format!(
                    "{}: duplicate module '{name}' (first declared at {})",
                    definition.location, previous.definition.location
                )
                .into());
            }
            let root = canonical(&directory.join("src"))?;
            if !root.is_dir() {
                return Err(format!(
                    "module source root '{}' must be a directory",
                    root.display()
                )
                .into());
            }
            modules.insert(
                name,
                IncludedModule {
                    manifest: file,
                    directory,
                    root,
                    definition,
                },
            );
        }

        let mut libraries = BTreeMap::new();
        for (name, target) in &manifest.targets {
            if let Some(module) = &target.module {
                if let Some(previous) = libraries.insert(module.clone(), name.clone()) {
                    return Err(format!(
                        "{}: targets '{previous}' and '{name}' both export module '{module}'",
                        path.display()
                    )
                    .into());
                }
                if target.entry.is_some() && modules.contains_key(module) {
                    return Err(format!(
                        "{}: library entry and included module both supply '{module}'",
                        path.display()
                    )
                    .into());
                }
                if target.entry.is_none() && !modules.contains_key(module) {
                    return Err(format!(
                        "{}: target '{name}' refers to unknown module '{module}'",
                        path.display()
                    )
                    .into());
                }
            }
        }
        for name in manifest.dependencies.keys() {
            if modules.contains_key(name) || libraries.contains_key(name) {
                return Err(format!(
                    "{}: local module and dependency both supply '{name}'",
                    path.display()
                )
                .into());
            }
        }
        Ok(Self {
            path: path.into(),
            directory,
            root,
            manifest,
            modules,
            libraries,
        })
    }

    fn select(&self, requested: Option<&str>) -> Result<Unit> {
        let targets = &self.manifest.targets;
        let name = requested
            .or(self.manifest.default_target.as_deref())
            .or_else(|| {
                (targets.len() == 1).then(|| targets.first_key_value().unwrap().0.as_str())
            });
        if let Some(name) = name.filter(|name| targets.contains_key(*name)) {
            return Ok(Unit::Target(name.into()));
        }
        let reason = match requested {
            Some(name) => format!("unknown target '{name}'"),
            None => "select a target".into(),
        };
        let choices = targets
            .keys()
            .map(String::as_str)
            .collect::<Vec<_>>()
            .join(", ");
        Err(format!(
            "{}: {reason}; available targets: {choices}",
            self.path.display()
        )
        .into())
    }

    fn resolve(&self, unit: &Unit) -> Result<(Project, Uses)> {
        let target = match unit {
            Unit::Target(name) => self.manifest.targets[name].clone(),
            Unit::LocalModule(name) => Target {
                kind: Kind::Library,
                module: Some(name.clone()),
                entry: None,
                uses: Uses::new(),
            },
        };
        let mut uses = target.uses;
        let module = target.module.unwrap_or_default();
        let entry = if let Some(relative) = target.entry {
            let file = source_file(&self.directory, &relative)?;
            if let Some(owner) = self.modules.values().find(|m| file.starts_with(&m.root)) {
                uses.extend(owner.definition.uses.clone());
            } else if !self
                .root
                .as_ref()
                .is_some_and(|root| file.starts_with(root))
            {
                return Err(format!(
                    "{}: entry '{}' is outside the project and included module src directories",
                    self.path.display(),
                    relative.display()
                )
                .into());
            }
            file
        } else {
            let owner = &self.modules[&module];
            let api = owner.definition.api.as_ref().ok_or_else(|| {
                format!(
                    "{}: module '{module}' requires an api for library use",
                    owner.manifest.display()
                )
            })?;
            let file = source_file(&owner.directory, api)?;
            if !file.starts_with(&owner.root) {
                return Err(format!(
                    "{}: api '{}' is outside the module src directory",
                    owner.manifest.display(),
                    api.display()
                )
                .into());
            }
            uses.extend(owner.definition.uses.clone());
            file
        };
        let project = Project {
            name: unit.name().into(),
            kind: target.kind,
            module,
            entry,
            output: self.directory.join(&self.manifest.output),
            dependencies: BTreeSet::new(),
            prebuilt: false,
        };
        Ok((project, uses))
    }
}

type Key = (PathBuf, Unit);

#[derive(Default)]
pub struct Graph {
    pub projects: Vec<Project>,
    packages: BTreeMap<PathBuf, Rc<Package>>,
    loaded: BTreeMap<Key, usize>,
    visiting: BTreeSet<Key>,
    providers: BTreeMap<String, Key>,
    inputs: BTreeSet<PathBuf>,
}

impl Graph {
    fn package(&mut self, path: &Path) -> Result<Rc<Package>> {
        let file = if path.is_dir() {
            canonical(&path.join("build.k"))?
        } else {
            canonical(path)?
        };
        if let Some(package) = self.packages.get(&file) {
            return Ok(Rc::clone(package));
        }
        let package = Rc::new(Package::read(&file)?);
        self.inputs.insert(file.clone());
        self.inputs
            .extend(package.modules.values().map(|m| m.manifest.clone()));
        self.packages.insert(file, Rc::clone(&package));
        Ok(package)
    }

    pub fn load(path: &Path, target: Option<&str>) -> Result<(Self, usize)> {
        let mut graph = Self::default();
        let package = graph.package(path)?;
        let unit = package.select(target)?;
        let index = graph.visit(&package, unit, false)?;
        Ok((graph, index))
    }

    pub fn check_destination(&self, artifact: &Path, target: &str) -> Result<()> {
        for package in self.packages.values() {
            let mut filenames = BTreeMap::new();
            for (name, declaration) in &package.manifest.targets {
                let mut file = declaration.kind.artifact_name(name, target);
                if cfg!(windows) {
                    file = file.to_lowercase();
                }
                if let Some(other) = filenames.insert(file, name) {
                    return Err(format!(
                        "{}: targets '{other}' and '{name}' have the same output filename",
                        package.path.display()
                    )
                    .into());
                }
            }
        }
        if !artifact.exists() {
            return Ok(());
        }
        let existing = canonical(artifact)?;
        if self.inputs.contains(&existing)
            || self.projects.iter().any(|project| {
                project.prebuilt
                    && dunce::canonicalize(project.artifact(target))
                        .is_ok_and(|path| path == existing)
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

    fn dependency(&mut self, package: &Rc<Package>, name: &str, location: &str) -> Result<usize> {
        if let Some(external) = package.manifest.dependencies.get(name) {
            let provider = self.package(&package.directory.join(&external.path))?;
            let target = provider.libraries.get(name).ok_or_else(|| {
                format!(
                    "{}: dependency '{name}' must declare a library target with that module name",
                    provider.path.display()
                )
            })?;
            return self.visit(&provider, Unit::Target(target.clone()), external.prebuilt);
        }
        if let Some(target) = package.libraries.get(name) {
            return self.visit(package, Unit::Target(target.clone()), false);
        }
        if package.modules.contains_key(name) {
            return self.visit(package, Unit::LocalModule(name.into()), false);
        }
        Err(format!("{location}: unknown module '{name}' in uses").into())
    }

    fn visit(&mut self, package: &Rc<Package>, unit: Unit, prebuilt: bool) -> Result<usize> {
        let key = (package.path.clone(), unit);
        if let Some(&index) = self.loaded.get(&key) {
            self.projects[index].prebuilt &= prebuilt;
            return Ok(index);
        }
        if !self.visiting.insert(key.clone()) {
            return Err(format!(
                "cyclic project dependency at {} ({})",
                package.path.display(),
                key.1.name()
            )
            .into());
        }
        let (mut project, uses) = package.resolve(&key.1)?;
        if project.kind == Kind::Library {
            if let Some(previous) = self.providers.insert(project.module.clone(), key.clone()) {
                return Err(format!(
                    "module '{}' is supplied by {} and {}",
                    project.module,
                    previous.0.display(),
                    package.path.display()
                )
                .into());
            }
        }
        self.inputs.insert(project.entry.clone());
        for (name, location) in uses {
            let index = self.dependency(package, &name, &location)?;
            project
                .dependencies
                .extend(&self.projects[index].dependencies);
            project.dependencies.insert(index);
        }
        project.prebuilt = prebuilt;
        self.visiting.remove(&key);
        let index = self.projects.len();
        self.projects.push(project);
        self.loaded.insert(key, index);
        Ok(index)
    }
}
