// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::{env, ffi::OsStr, process::ExitCode};

fn main() -> ExitCode {
    if env::args_os().nth(1).as_deref() == Some(OsStr::new("build")) {
        let make = if cfg!(windows) {
            "mingw32-make"
        } else {
            "make"
        };
        eprintln!("Shuttle object emission is unavailable.");
        eprintln!(
            "To check the standard library, run `{make} run` from the Cloth repository root."
        );
    } else {
        eprintln!("Usage: shuttle build");
    }
    ExitCode::FAILURE
}
