// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

mod build;
mod manifest;
mod project;

use std::{env, error::Error, ffi::OsString, path::PathBuf, process::ExitCode};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

const HELP: &str = "\
Shuttle builds Cloth executables and libraries from build.k.

Usage
  shuttle build [TARGET] [OPTIONS]
  shuttle --help

Examples
  shuttle build                 Build the default target in this directory
  shuttle build arithmetic-lib  Build the calculator example's library

Project options
  --manifest-path PATH   Use a build.k file or a project directory
  --target-bits 32|64    Build for 32-bit or 64-bit x86

Toolchain options
  --compiler PATH        Use this Cloth compiler executable
  --cc PATH              Link the executable with this C compiler driver
  --stdlib DIR           Read standard library sources from DIR
  --runtime-dir DIR      Look for DIR/<target-triple>/libcloth_runtime.a";

struct Options {
    manifest: PathBuf,
    target: Option<String>,
    compiler: PathBuf,
    linker: PathBuf,
    runtime: Option<PathBuf>,
    stdlib: Option<PathBuf>,
    bits: Option<String>,
}

impl Options {
    fn parse(mut args: impl Iterator<Item = OsString>) -> Result<Option<Self>> {
        let mut options = Self {
            manifest: "build.k".into(),
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
                Some("--manifest-path") => options.manifest = value()?.into(),
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
                Some(name) if !name.starts_with('-') => {
                    if options.target.replace(name.to_owned()).is_some() {
                        return Err("shuttle build accepts one target name".into());
                    }
                }
                _ => {
                    return Err(format!("unknown build option '{}'", flag.to_string_lossy()).into());
                }
            }
        }
        Ok(Some(options))
    }
}

fn run() -> Result<()> {
    let mut args = env::args_os().skip(1);
    let Some(command) = args.next() else {
        return Err(format!("expected a command\n\n{HELP}").into());
    };
    let options = match command.to_str() {
        Some("build") => Options::parse(args)?,
        Some("-h" | "--help") => None,
        _ => {
            return Err(
                format!("unknown command '{}'\n\n{HELP}", command.to_string_lossy()).into(),
            );
        }
    };
    if let Some(options) = options {
        let artifact = build::project(options)?;
        println!("{}", artifact.display());
    } else {
        println!("{HELP}");
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
