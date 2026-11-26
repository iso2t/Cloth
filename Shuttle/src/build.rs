// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::manifest::Kind;
use crate::project::{Graph, Project};
use crate::{Options, Result};
use std::env;
use std::ffi::OsString;
use std::fs;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

struct Staging(PathBuf);

impl Staging {
    fn create(directory: &Path) -> Result<Self> {
        fs::create_dir_all(directory)?;
        for attempt in 0.. {
            let path = directory.join(format!(".shuttle-{}-{attempt}", process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(Self(path)),
                Err(error) if error.kind() == ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error.into()),
            }
        }
        unreachable!()
    }
}

impl Drop for Staging {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

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
        }
    }
    if let Some(stdlib) = &options.stdlib {
        command.arg("--stdlib").arg(stdlib);
    }
    for &index in &project.dependencies {
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

fn link(options: &Options, target: &str, objects: &[PathBuf], output: &Path) -> Result<()> {
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
    for (id, object) in objects.iter().enumerate() {
        let name = format!("{id}.o");
        link_input(object, directory, &name)?;
        command.arg(name);
    }
    link_input(&archive, directory, "libcloth_runtime.a")?;
    command.arg("libcloth_runtime.a");
    if target.contains("mingw") || target.contains("windows") {
        command.arg("-lshell32");
    } else {
        command.arg("-lm");
    }
    checked(command.arg("-o").arg(output.file_name().unwrap()))
}

pub fn project(mut options: Options) -> Result<PathBuf> {
    let (graph, root) = Graph::load(&options.manifest, options.target.as_deref())?;
    options.compiler = find_compiler(&options.compiler)?;
    let target = target(&options)?;
    let project = &graph.projects[root];
    let artifact = project.artifact(&target);
    graph.check_destination(&artifact, &target)?;
    let staging = Staging::create(artifact.parent().unwrap())?;
    let output = staging.0.join("output").with_extension(
        artifact
            .extension()
            .filter(|ext| *ext == "exe")
            .unwrap_or_default(),
    );
    if project.kind == Kind::Library {
        compile(&options, &graph.projects, project, &output)?;
    } else {
        let objects = compile_objects(&options, &graph.projects, &target, &staging)?;
        link(&options, &target, &objects, &output)?;
    }
    fs::rename(output, &artifact)
        .map_err(|error| format!("publishing {}: {error}", artifact.display()))?;
    Ok(artifact)
}

fn compile_objects(
    options: &Options,
    projects: &[Project],
    target: &str,
    staging: &Staging,
) -> Result<Vec<PathBuf>> {
    let mut objects = Vec::new();
    for (id, dependency) in projects.iter().enumerate() {
        let object = if dependency.prebuilt {
            let path = dependency.artifact(target);
            if !path.is_file() {
                return Err(format!(
                    "missing prebuilt library for {target} at {}",
                    path.display()
                )
                .into());
            }
            path
        } else {
            let path = staging.0.join(format!("{id}.o"));
            compile(options, projects, dependency, &path)?;
            path
        };
        objects.push(object);
    }
    Ok(objects)
}
