// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

mod build;
mod init;
mod interface;
mod manifest;
mod project;
mod staging;

use std::{
    env,
    error::Error,
    ffi::OsString,
    path::PathBuf,
    process::{Command, ExitCode},
};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

const HELP: &str = "\
Shuttle creates, builds, and runs Cloth projects from build.k.

Usage
  shuttle init [PATH]
  shuttle init --module NAME
  shuttle build [TARGET] [OPTIONS]
  shuttle run [PATH] [--target NAME] [OPTIONS] [-- ARGS...]
  shuttle --help

Examples
  shuttle init hello            Create a runnable project in hello/
  shuttle init --module Math    Add Math to the enclosing Cloth project
  shuttle build                 Build the default target in this directory
  shuttle build arithmetic-lib  Build the calculator example's library
  shuttle run                   Build the enclosing project and run its executable
  shuttle run examples/calculator -- 6 + 7

Project options
  --manifest-path PATH   Use a build.k file or a project directory
  --target-bits 32|64    Build for 32-bit or 64-bit x86
  --target NAME          Select the executable to launch with shuttle run

Toolchain options
  --compiler PATH        Use this Cloth compiler executable
  --cc PATH              Link the executable with this C compiler driver
  --stdlib DIR           Read standard library sources from DIR
  --runtime-dir DIR      Look for DIR/<target-triple>/libcloth_runtime.a";

#[derive(Clone, Copy, Eq, PartialEq)]
enum Action {
    Build,
    Run,
}

struct Options {
    manifest: Option<PathBuf>,
    target: Option<String>,
    compiler: PathBuf,
    linker: PathBuf,
    runtime: Option<PathBuf>,
    stdlib: Option<PathBuf>,
    bits: Option<String>,
}

impl Options {
    fn set_manifest(&mut self, path: OsString, action: Action) -> Result<()> {
        if self.manifest.replace(path.into()).is_some() && action == Action::Run {
            return Err("shuttle run accepts one project path".into());
        }
        Ok(())
    }

    fn parse(action: Action, args: &mut impl Iterator<Item = OsString>) -> Result<Option<Self>> {
        let mut options = Self {
            manifest: None,
            target: None,
            compiler: env::var_os("CLOTHC").map_or_else(|| "cloth".into(), PathBuf::from),
            linker: env::var_os("CC").map_or_else(
                || {
                    if cfg!(windows) {
                        "gcc".into()
                    } else {
                        "cc".into()
                    }
                },
                PathBuf::from,
            ),
            runtime: env::var_os("CLOTH_RUNTIME_DIR").map(PathBuf::from),
            stdlib: env::var_os("CLOTH_STDLIB").map(PathBuf::from),
            bits: None,
        };
        while let Some(flag) = args.next() {
            let mut value = || {
                args.next()
                    .ok_or_else(|| format!("{} requires a value", flag.to_string_lossy()))
            };
            match flag.to_str() {
                Some("-h" | "--help") => return Ok(None),
                Some("--") if action == Action::Run => break,
                Some("--manifest-path") => options.set_manifest(value()?, action)?,
                Some("--target") if action == Action::Run => {
                    let name = value()?
                        .into_string()
                        .map_err(|_| "target names must be UTF-8")?;
                    if options.target.replace(name).is_some() {
                        return Err("shuttle run accepts one executable target".into());
                    }
                }
                Some("--compiler") => options.compiler = value()?.into(),
                Some("--cc") => options.linker = value()?.into(),
                Some("--stdlib") => options.stdlib = Some(value()?.into()),
                Some("--runtime-dir") => options.runtime = Some(value()?.into()),
                Some("--target-bits") => {
                    let value = value()?;
                    if value != "32" && value != "64" {
                        return Err("--target-bits requires 32 or 64".into());
                    }
                    options.bits = value.into_string().ok();
                }
                _ if action == Action::Run && !flag.as_encoded_bytes().starts_with(b"-") => {
                    options.set_manifest(flag, action)?;
                }
                Some(name) if action == Action::Build && !name.starts_with('-') => {
                    if options.target.replace(name.to_owned()).is_some() {
                        return Err("shuttle build accepts one target name".into());
                    }
                }
                _ => {
                    let command = if action == Action::Run {
                        "run"
                    } else {
                        "build"
                    };
                    return Err(
                        format!("unknown {command} option '{}'", flag.to_string_lossy()).into(),
                    );
                }
            }
        }
        Ok(Some(options))
    }
}

fn launch(artifact: build::Artifact, args: impl Iterator<Item = OsString>) -> Result<()> {
    let mut program = Command::new(&artifact.path);
    program.current_dir(artifact.directory).args(args);
    #[cfg(unix)]
    let error = std::os::unix::process::CommandExt::exec(&mut program);
    #[cfg(not(unix))]
    let error = match program.status() {
        Ok(status) => std::process::exit(status.code().unwrap_or(1)),
        Err(error) => error,
    };
    Err(format!("running {}: {error}", artifact.path.display()).into())
}

fn run() -> Result<()> {
    let mut args = env::args_os().skip(1);
    let Some(command) = args.next() else {
        return Err(format!("expected a command\n\n{HELP}").into());
    };
    let action = match command.to_str() {
        Some("init") => return init::run(args),
        Some("build") => Action::Build,
        Some("run") => Action::Run,
        Some("-h" | "--help") => {
            println!("{HELP}");
            return Ok(());
        }
        _ => {
            return Err(
                format!("unknown command '{}'\n\n{HELP}", command.to_string_lossy()).into(),
            );
        }
    };
    let Some(options) = Options::parse(action, &mut args)? else {
        println!("{HELP}");
        return Ok(());
    };
    let artifact = build::project(options, action)?;
    match action {
        Action::Build => println!("{}", artifact.path.display()),
        Action::Run => return launch(artifact, args),
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("shuttle: {error}");
            ExitCode::FAILURE
        }
    }
}
