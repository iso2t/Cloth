// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

mod build;
mod manifest;
mod project;

use std::{env, error::Error, ffi::OsStr, path::PathBuf, process::ExitCode};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

const USAGE: &str = "Usage: shuttle build [TARGET] [--manifest-path PATH] [--target-bits 32|64] [--stdlib DIR] [--compiler PATH] [--cc PATH] [--runtime-dir DIR]";

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
    fn parse(mut args: impl Iterator<Item = std::ffi::OsString>) -> Result<Self> {
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
        Ok(options)
    }
}

fn run() -> Result<()> {
    let mut args = env::args_os().skip(1);
    if args.next().as_deref() != Some(OsStr::new("build")) {
        return Err(USAGE.into());
    }
    let artifact = build::project(Options::parse(args)?)?;
    println!("{}", artifact.display());
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
