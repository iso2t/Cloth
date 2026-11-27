// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::interface::Interfaces;
use crate::manifest::Kind;
use crate::project::{self, Graph, Project};
use crate::staging::Staging;
use crate::{Action, Options, Result};
use std::env;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

fn executable(path: &Path) -> Option<PathBuf> {
    if path.is_file() {
        return Some(path.to_owned());
    }
    if cfg!(windows) && path.extension().is_none() {
        let candidate = path.with_extension("exe");
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

fn find_compiler(name: &Path) -> Result<PathBuf> {
    let found = if name.components().count() > 1 {
        executable(name)
    } else {
        env::var_os("PATH")
            .and_then(|path| env::split_paths(&path).find_map(|dir| executable(&dir.join(name))))
            .or_else(|| executable(&env::current_exe().ok()?.parent()?.join(name)))
    };
    let path = found.ok_or_else(|| format!("cannot find Cloth compiler '{}'", name.display()))?;
    Ok(dunce::canonicalize(path)?)
}

fn checked(command: &mut Command) -> Result<()> {
    let program = command.get_program().to_string_lossy().into_owned();
    let status = command
        .status()
        .map_err(|error| format!("{program}: {error}"))?;
    if !status.success() {
        return Err(format!("{program} exited with {status}").into());
    }
    Ok(())
}

fn compiler(options: &Options) -> Command {
    let mut command = Command::new(&options.compiler);
    if let Some(bits) = &options.bits {
        command.args(["--target-bits", bits]);
    }
    command
}

fn compile(
    options: &Options,
    projects: &[Project],
    project: &Project,
    output: &Path,
) -> Result<()> {
    let mut command = compiler(options);
    command.arg("--src").arg(&project.entry);
    match project.kind {
        Kind::Library => {
            command.args(["--kind", "library", "--module-name", &project.module]);
        }
        Kind::Executable => {
            command.args(["--kind", "executable"]);
            if project.dependencies.values().any(|&prebuilt| prebuilt) {
                command.arg("--record-extern-interfaces");
            }
        }
    }
    if let Some(stdlib) = &options.stdlib {
        command.arg("--stdlib").arg(stdlib);
    }
    for &index in project.dependencies.keys() {
        let library = &projects[index];
        let mut binding = OsString::from(&library.module);
        binding.push("=");
        binding.push(&library.entry);
        command.arg("--extern").arg(binding);
    }
    checked(command.arg("--emit-object").arg(output))
}

fn target(options: &Options) -> Result<String> {
    let output = compiler(options)
        .arg("--print-target")
        .stderr(process::Stdio::inherit())
        .output()?;
    if !output.status.success() {
        return Err("the Cloth compiler could not identify its target".into());
    }
    let triple = std::str::from_utf8(&output.stdout)?.trim();
    if triple.is_empty()
        || !triple
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"-_.".contains(&b))
        || !triple.contains('-')
    {
        return Err(format!("invalid compiler target '{triple}'").into());
    }
    Ok(triple.into())
}

// Native linkers can interpret path arguments through the Windows ANSI code page.
// Pass ASCII filenames in a native working directory and keep published inputs intact.
fn link_input(source: &Path, directory: &Path, name: &str) -> Result<()> {
    let staged = directory.join(name);
    if source != staged && fs::hard_link(source, &staged).is_err() {
        fs::copy(source, &staged)
            .map_err(|error| format!("staging {}: {error}", source.display()))?;
    }
    Ok(())
}

struct LibraryInput<'a> {
    project: &'a Project,
    object: PathBuf,
    prebuilt: bool,
}

impl LibraryInput<'_> {
    fn stage(&self, interfaces: Option<&Interfaces>, directory: &Path, name: &str) -> Result<()> {
        let Some(expected) = interfaces else {
            return link_input(&self.object, directory, name);
        };
        let bytes = fs::read(&self.object)
            .map_err(|error| format!("reading {}: {error}", self.object.display()))?;
        expected
            .check_library(&self.project.module, &bytes)
            .map_err(|error| {
                if self.prebuilt {
                    format!(
                        "{error}. Please rebuild prebuilt library '{}' (target '{}' in {}).",
                        self.project.module,
                        self.project.name,
                        self.project.manifest.display()
                    )
                } else {
                    format!(
                        "{error} while building source library '{}'",
                        self.project.module
                    )
                }
            })?;
        // The linker must see the bytes we checked even if the provider replaces or rewrites its object.
        fs::write(directory.join(name), bytes)?;
        Ok(())
    }
}

fn link(
    options: &Options,
    target: &str,
    libraries: &[LibraryInput],
    entry: &Path,
    output: &Path,
) -> Result<()> {
    let root = options
        .runtime
        .clone()
        .unwrap_or_else(|| options.compiler.parent().unwrap().join("lib/cloth"));
    let archive = root.join(target).join("libcloth_runtime.a");
    if !archive.is_file() {
        return Err(format!(
            "runtime for {target} was not found at {}",
            archive.display()
        )
        .into());
    }
    let directory = output.parent().unwrap();
    let linker = if options.linker.components().count() > 1 {
        std::path::absolute(&options.linker)?
    } else {
        options.linker.clone()
    };
    let mut command = Command::new(linker);
    command.current_dir(directory);
    match target.split('-').next() {
        Some("x86_64" | "amd64") => {
            command.arg("-m64");
        }
        Some("i386" | "i486" | "i586" | "i686") => {
            command.arg("-m32");
        }
        _ => {}
    }
    let entry_bytes = if libraries.iter().any(|library| library.prebuilt) {
        Some(fs::read(entry)?)
    } else {
        None
    };
    let interfaces = entry_bytes.as_deref().map(Interfaces::read).transpose()?;
    for (id, library) in libraries.iter().enumerate() {
        let name = format!("{id}.o");
        library.stage(interfaces.as_ref(), directory, &name)?;
        command.arg(name);
    }
    link_input(entry, directory, "main.o")?;
    command.arg("main.o");
    link_input(&archive, directory, "libcloth_runtime.a")?;
    command.arg("libcloth_runtime.a");
    if target.contains("mingw") || target.contains("windows") {
        command.arg("-lshell32");
    } else {
        command.arg("-lm");
    }
    checked(command.arg("-o").arg(output.file_name().unwrap()))
}

pub struct Artifact {
    pub path: PathBuf,
    pub directory: PathBuf,
}

pub fn project(mut options: Options, action: Action) -> Result<Artifact> {
    let manifest = match options.manifest.take() {
        None => project::enclosing_manifest(Path::new("."))?,
        Some(path) if action == Action::Run && path.is_dir() => project::enclosing_manifest(&path)?,
        Some(path) => path,
    };
    let (graph, root) = Graph::load(&manifest, options.target.as_deref(), action)?;
    options.compiler = find_compiler(&options.compiler)?;
    let target = target(&options)?;
    let selected = &graph.projects[root];
    graph.check_destinations(
        &target,
        action == Action::Run || selected.kind == Kind::Executable,
    )?;
    let path = selected.artifact(&target);
    let staging = Staging::create(path.parent().unwrap())?;
    let object = |index: usize| staging.path.join(format!("{index}.o"));
    for &index in &graph.sources {
        compile(
            &options,
            &graph.projects,
            &graph.projects[index],
            &object(index),
        )?;
    }

    let mut outputs = Vec::new();
    for &index in &graph.targets {
        let project = &graph.projects[index];
        let artifact = project.artifact(&target);
        let output = match project.kind {
            Kind::Library => object(index),
            Kind::Executable => {
                let directory = staging.path.join(format!("link-{index}"));
                fs::create_dir(&directory)?;
                let output = directory.join("output").with_extension(
                    artifact
                        .extension()
                        .filter(|ext| *ext == "exe")
                        .unwrap_or_default(),
                );
                let mut libraries = Vec::new();
                for (&dependency, &prebuilt) in &project.dependencies {
                    let library = &graph.projects[dependency];
                    libraries.push(LibraryInput {
                        project: library,
                        object: if prebuilt {
                            library.artifact(&target)
                        } else {
                            object(dependency)
                        },
                        prebuilt,
                    });
                }
                link(&options, &target, &libraries, &object(index), &output)?;
                output
            }
        };
        if !output.is_file() {
            return Err(format!("no staged file for {}", artifact.display()).into());
        }
        outputs.push((output, artifact));
    }
    staging.publish(&selected.output, &outputs)?;
    Ok(Artifact {
        path,
        directory: selected.manifest.parent().unwrap().to_owned(),
    })
}
