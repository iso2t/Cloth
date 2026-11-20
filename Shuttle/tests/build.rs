// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::{fs, path::Path, process::Command};

#[test]
fn object_build_reports_the_native_check_and_leaves_the_directory_empty() {
    let directory =
        Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("object-build-{}", std::process::id()));
    fs::create_dir(&directory).unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_shuttle"))
        .args(["build", "--stdlib", ".", "--emit", "object"])
        .current_dir(&directory)
        .output()
        .unwrap();
    fs::remove_dir(&directory).expect("the build should leave its output directory empty");

    assert!(!output.status.success());
    assert!(output.stdout.is_empty());
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(
        stderr.contains("object emission is unavailable"),
        "{stderr}"
    );
    let native_check = if cfg!(windows) {
        "`mingw32-make run`"
    } else {
        "`make run`"
    };
    assert!(stderr.contains(native_check), "{stderr}");
}
