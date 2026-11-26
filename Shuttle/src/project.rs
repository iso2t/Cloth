// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::manifest::{Kind, Manifest, Module, Target, Uses};
use crate::{Action, Result};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Component, Path, PathBuf};
use std::rc::Rc;
use std::{fs, io::ErrorKind};

pub struct Project {
    pub manifest: PathBuf,
    pub name: String,
    pub module: String,
    pub kind: Kind,
    pub entry: PathBuf,
    pub output: PathBuf,
    // A target links a prebuilt object only if every path to that dependency requests it.
    pub dependencies: BTreeMap<usize, bool>,
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

    fn add_dependency(&mut self, index: usize, prebuilt: bool) {
        *self.dependencies.entry(index).or_insert(true) &= prebuilt;
    }
}

fn canonical(path: &Path) -> Result<PathBuf> {
    dunce::canonicalize(path).map_err(|error| format!("{}: {error}", path.display()).into())
}

pub fn enclosing_manifest(directory: &Path) -> Result<PathBuf> {
    let directory = canonical(directory)?;
    for dir in directory.ancestors() {
        let path = dir.join("build.k");
        match fs::symlink_metadata(&path) {
            Ok(_) => return canonical(&path),
            Err(e) if e.kind() == ErrorKind::NotFound => continue,
            Err(e) => return Err(format!("{}: {e}", path.display()).into()),
        }
    }
    Err(format!("no build.k in {} or its parents", directory.display()).into())
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

pub fn check_new_module(path: &Path, name: &str) -> Result<()> {
    let package = Package::read(path)?;
    if package.modules.contains_key(name)
        || package.libraries.contains_key(name)
        || package.manifest.dependencies.contains_key(name)
    {
        return Err(format!("{}: module name '{name}' is already in use", path.display()).into());
    }
    Ok(())
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

    fn select(&self, requested: Option<&str>, action: Action) -> Result<Unit> {
        let targets = &self.manifest.targets;
        let choices: Vec<_> = targets
            .iter()
            .filter(|(_, target)| action == Action::Build || target.kind == Kind::Executable)
            .map(|(name, _)| name.as_str())
            .collect();
        let default = self
            .manifest
            .default_target
            .as_deref()
            .filter(|n| choices.contains(n));
        let name = requested.or(default).or(match choices.as_slice() {
            [only] => Some(*only),
            _ => None,
        });
        if let Some(name) = name {
            if choices.contains(&name) {
                return Ok(Unit::Target(name.into()));
            }
            if targets.contains_key(name) {
                return Err(
                    format!("{}: target '{name}' is a library", self.path.display()).into(),
                );
            }
        }
        if choices.is_empty() {
            return Err(
                format!("{}: project has no executable targets", self.path.display()).into(),
            );
        }
        let (kind, prompt) = match action {
            Action::Build => ("target", "select a target"),
            Action::Run => ("executable", "select an executable with --target NAME"),
        };
        let reason = requested.map_or_else(
            || prompt.to_owned(),
            |name| format!("unknown {kind} '{name}'"),
        );
        Err(format!(
            "{}: {reason}; available {kind}s: {}",
            self.path.display(),
            choices.join(", ")
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
            manifest: self.path.clone(),
            name: unit.name().into(),
            kind: target.kind,
            module,
            entry,
            output: self.directory.join(&self.manifest.output),
            dependencies: BTreeMap::new(),
        };
        Ok((project, uses))
    }
}

type Key = (PathBuf, Unit);

#[derive(Default)]
pub struct Graph {
    pub projects: Vec<Project>,
    pub targets: Vec<usize>,
    pub sources: BTreeSet<usize>,
    prebuilt: BTreeSet<usize>,
    packages: BTreeMap<PathBuf, Rc<Package>>,
    loaded: BTreeMap<Key, usize>,
    visiting: BTreeSet<Key>,
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

    pub fn load(path: &Path, target: Option<&str>, action: Action) -> Result<(Self, usize)> {
        let mut graph = Self::default();
        let package = graph.package(path)?;
        let unit = package.select(target, action)?;
        let index = graph.plan(&package, unit.clone(), action)?;
        if action == Action::Run {
            for name in package
                .manifest
                .targets
                .keys()
                .filter(|name| *name != unit.name())
            {
                graph.plan(&package, Unit::Target(name.clone()), action)?;
            }
            for (name, module) in &package.modules {
                if module.definition.api.is_some() && !package.libraries.contains_key(name) {
                    graph.plan(&package, Unit::LocalModule(name.clone()), action)?;
                }
            }
        }
        Ok((graph, index))
    }

    fn plan(&mut self, package: &Rc<Package>, unit: Unit, action: Action) -> Result<usize> {
        let publish = matches!(unit, Unit::Target(_));
        let index = self.visit(package, unit)?;
        if publish {
            self.targets.push(index);
        }
        self.sources.insert(index);
        let project = &self.projects[index];
        for (&dependency, &prebuilt) in &project.dependencies {
            if prebuilt {
                self.prebuilt.insert(dependency);
            } else if action == Action::Run || project.kind == Kind::Executable {
                self.sources.insert(dependency);
            }
        }
        Ok(index)
    }

    pub fn check_destinations(&self, target: &str, need_prebuilt: bool) -> Result<()> {
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
        let mut prebuilt_inputs = BTreeSet::new();
        for &index in &self.prebuilt {
            let path = self.projects[index].artifact(target);
            if path.is_file() {
                prebuilt_inputs.insert(canonical(&path)?);
            } else if need_prebuilt {
                return Err(format!(
                    "missing prebuilt library for {target} at {}",
                    path.display()
                )
                .into());
            }
        }
        for &index in &self.targets {
            let artifact = self.projects[index].artifact(target);
            if artifact.try_exists()? {
                let existing = canonical(&artifact)?;
                if self.inputs.contains(&existing) || prebuilt_inputs.contains(&existing) {
                    return Err(format!(
                        "output would overwrite a project input at {}",
                        artifact.display()
                    )
                    .into());
                }
            }
        }
        Ok(())
    }

    fn dependency(
        &mut self,
        package: &Rc<Package>,
        name: &str,
        location: &str,
    ) -> Result<(usize, bool)> {
        if let Some(external) = package.manifest.dependencies.get(name) {
            let provider = self.package(&package.directory.join(&external.path))?;
            let target = provider.libraries.get(name).ok_or_else(|| {
                format!(
                    "{}: dependency '{name}' must declare a library target with that module name",
                    provider.path.display()
                )
            })?;
            return Ok((
                self.visit(&provider, Unit::Target(target.clone()))?,
                external.prebuilt,
            ));
        }
        if let Some(target) = package.libraries.get(name) {
            return Ok((self.visit(package, Unit::Target(target.clone()))?, false));
        }
        if package.modules.contains_key(name) {
            return Ok((self.visit(package, Unit::LocalModule(name.into()))?, false));
        }
        Err(format!("{location}: unknown module '{name}' in uses").into())
    }

    fn visit(&mut self, package: &Rc<Package>, unit: Unit) -> Result<usize> {
        let key = (package.path.clone(), unit);
        if let Some(&index) = self.loaded.get(&key) {
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
        self.inputs.insert(project.entry.clone());
        for (name, location) in uses {
            let (index, prebuilt) = self.dependency(package, &name, &location)?;
            for (&dependency, &prebuilt) in &self.projects[index].dependencies {
                project.add_dependency(dependency, prebuilt);
            }
            project.add_dependency(index, prebuilt);
        }
        // Executables may use independent providers of the same import name.
        // Each target's own dependency closure must still have one provider per name.
        let mut providers = BTreeMap::new();
        for library in project
            .dependencies
            .keys()
            .map(|&i| &self.projects[i])
            .chain(std::iter::once(&project))
            .filter(|p| p.kind == Kind::Library)
        {
            if let Some(previous) = providers.insert(&library.module, &library.manifest) {
                return Err(format!(
                    "module '{}' is supplied by {} and {}",
                    library.module,
                    previous.display(),
                    library.manifest.display()
                )
                .into());
            }
        }
        self.visiting.remove(&key);
        let index = self.projects.len();
        self.projects.push(project);
        self.loaded.insert(key, index);
        Ok(index)
    }
}
