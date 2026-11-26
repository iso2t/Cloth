// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const API: &str = include_str!("../../Tests/Compiler/Driver/Inputs/native-library/api.co");
const MAIN: &str = include_str!("../../Tests/Compiler/Driver/Inputs/native-library/main.co");
const OFFSETS: &str = include_str!("../../Tests/Compiler/Driver/Inputs/native-library/offsets.co");
const COLLECTIONS: &str = include_str!("../../Tests/Compiler/Driver/Inputs/stdlib-collections.co");

struct Directory(PathBuf);

impl Directory {
    fn new(name: &str) -> Self {
        for attempt in 0.. {
            let path = Path::new(env!("CARGO_TARGET_TMPDIR"))
                .join(format!("{name} {} {attempt}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("{error}"),
            }
        }
        unreachable!()
    }
}

impl Drop for Directory {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}

fn build(dir: &Path) -> Command {
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let compiler = env::var_os("CLOTHC").map(PathBuf::from).unwrap_or_else(|| {
        repo.join("build")
            .join(if cfg!(windows) { "cloth.exe" } else { "cloth" })
    });
    let mut command = Command::new(env!("CARGO_BIN_EXE_shuttle"));
    command
        .current_dir(dir)
        .arg("build")
        .arg("--compiler")
        .arg(compiler);
    if let Ok(bits) = env::var("SHUTTLE_TEST_TARGET_BITS") {
        command.args(["--target-bits", &bits]);
    }
    command
}

fn artifact(command: &mut Command) -> PathBuf {
    let output = command.output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let path = PathBuf::from(std::str::from_utf8(&output.stdout).unwrap().trim());
    assert!(path.is_file(), "{}", path.display());
    path
}

fn failure(output: Output, expected: &str) {
    assert!(!output.status.success());
    let error = String::from_utf8_lossy(&output.stderr);
    assert!(error.contains(expected), "{error}");
}

fn run(path: &Path, code: i32, stdout: &str) {
    let output = Command::new(path).output().unwrap();
    assert_eq!(
        output.status.code(),
        Some(code),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        String::from_utf8(output.stdout)
            .unwrap()
            .replace("\r\n", "\n"),
        stdout
    );
    assert!(output.stderr.is_empty());
}

#[test]
fn separate_objects_rebuild_and_preserve_the_previous_executable_on_failure() {
    let work = Directory::new("native projects π");
    let api = work.0.join("api");
    let app = work.0.join("app");
    fs::create_dir_all(api.join("src")).unwrap();
    fs::create_dir_all(&app).unwrap();
    fs::write(api.join("src/api.co"), API).unwrap();
    fs::write(api.join("src/offsets.co"), OFFSETS).unwrap();
    fs::write(app.join("main.co"), MAIN).unwrap();
    fs::write(app.join("offsets.co"), "pub fn<int> offset() = 99;\n").unwrap();
    fs::write(api.join("build.toml"), "[project]\nname='arithmetic-lib'\nmodule='Arithmetic'\nkind='library'\nentryPoint='Api.api'\n[modules]\nApi='src'\n").unwrap();
    let manifest = "[project]\nname='answer'\nentryPoint='App.main.main'\n[modules]\nApp='.'\n[dependencies]\nArithmetic=";
    fs::write(
        app.join("build.toml"),
        format!("{manifest}{{project='../api',prebuilt=true}}\n"),
    )
    .unwrap();
    failure(build(&app).output().unwrap(), "missing prebuilt library");
    let library = artifact(&mut build(&api));
    let executable = artifact(&mut build(&app));
    assert_eq!(
        library.parent().unwrap().file_name(),
        executable.parent().unwrap().file_name()
    );
    run(&executable, 42, "");

    fs::write(api.join("src/offsets.co"), "pub fn<int> offset() = 3;\n").unwrap();
    run(&artifact(&mut build(&app)), 42, "");
    artifact(&mut build(&api));
    run(&artifact(&mut build(&app)), 43, "");
    fs::write(api.join("src/offsets.co"), "pub fn<int> offset() = 4;\n").unwrap();
    fs::write(app.join("build.toml"), format!("{manifest}'../api'\n")).unwrap();
    run(&artifact(&mut build(&app)), 44, "");

    let wrong = work.0.join("runtime");
    fs::create_dir_all(wrong.join("other-target")).unwrap();
    fs::write(wrong.join("other-target/libcloth_runtime.a"), []).unwrap();
    let target = executable
        .parent()
        .unwrap()
        .file_name()
        .unwrap()
        .to_str()
        .unwrap();
    failure(
        build(&app)
            .arg("--runtime-dir")
            .arg(&wrong)
            .output()
            .unwrap(),
        &format!("runtime for {target} was not found"),
    );
    run(&executable, 44, "");
    fs::write(
        app.join("main.co"),
        "using Arithmetic : adjust;\npub fn<int> main() = adjust(40);\n",
    )
    .unwrap();
    failure(build(&app).output().unwrap(), "'adjust' is private");
    run(&executable, 44, "");
}

#[test]
fn collections_project_links_the_packaged_runtime() {
    let work = Directory::new("collections");
    fs::write(work.0.join("main.co"), COLLECTIONS).unwrap();
    fs::write(
        work.0.join("build.toml"),
        "[project]\nname='collections'\nentry='main.co'\n[output]\ndirectory='out'\n",
    )
    .unwrap();
    let stdlib = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("Stdlib");
    let path = artifact(build(&work.0).arg("--stdlib").arg(stdlib));
    assert!(
        path.canonicalize()
            .unwrap()
            .starts_with(work.0.canonicalize().unwrap().join("out"))
    );
    run(&path, 42, "40\n2\n");
}

#[test]
fn rejects_a_cyclic_project_before_creating_output() {
    let work = Directory::new("cycle");
    fs::write(
        work.0.join("build.toml"),
        "[project]\nname='Loop'\nkind='library'\nentry='api.co'\n[dependencies]\nLoop='.'\n",
    )
    .unwrap();
    failure(
        build(&work.0).output().unwrap(),
        "cyclic project dependency",
    );
    assert!(!work.0.join("build").exists());
}

#[test]
fn transitive_libraries_keep_identical_signatures_in_their_own_modules() {
    let work = Directory::new("module boundaries");
    let cases = [
        ("Left", "pub fn<int> value() = 40;\n", ""),
        ("Right", "pub fn<int> value() = 2;\n", ""),
        (
            "First",
            "using Left : value;\npub fn<int> first() = value();\n",
            "Left='../Left'\n",
        ),
        (
            "Second",
            "using Right : value;\npub fn<int> second() = value();\n",
            "Right='../Right'\n",
        ),
    ];
    for (name, source, dependencies) in cases {
        let dir = work.0.join(name);
        fs::create_dir(&dir).unwrap();
        fs::write(dir.join("api.co"), source).unwrap();
        fs::write(dir.join("build.toml"), format!("[project]\nname='{name}'\nkind='library'\nentry='api.co'\n[dependencies]\n{dependencies}")).unwrap();
    }
    fs::write(
        work.0.join("main.co"),
        "using First : first;\nusing Second : second;\npub fn<int> main() = first() + second();\n",
    )
    .unwrap();
    fs::write(
        work.0.join("build.toml"),
        "[project]\nname='sum'\nentry='main.co'\n[dependencies]\nFirst='First'\nSecond='Second'\n",
    )
    .unwrap();
    run(&artifact(&mut build(&work.0)), 42, "");
}
