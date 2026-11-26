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

fn write(root: &Path, relative: &str, contents: &str) {
    let path = root.join(relative);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, contents).unwrap();
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
    write(&api, "src/api.co", API);
    write(&api, "src/offsets.co", OFFSETS);
    write(&app, "src/main.co", MAIN);
    write(&app, "src/offsets.co", "pub fn<int> offset() = 99;\n");
    write(
        &api,
        "build.k",
        r#"project "tools" {
        target "executable" entry="src/tool.co"
        target "library" entry="src/api.co" name="arithmetic-lib" module="Arithmetic"
    }"#,
    );
    let consumer = |prebuilt: bool| {
        format!(
            r#"project "answer" {{
        dependency "Arithmetic" path="../api" prebuilt=#{prebuilt}
        target "executable" entry="src/main.co" {{
            uses "Arithmetic"
        }}
    }}"#
        )
    };
    write(&app, "build.k", &consumer(true));
    failure(build(&app).output().unwrap(), "missing prebuilt library");
    let library = artifact(build(&api).arg("arithmetic-lib"));
    let executable = artifact(&mut build(&app));
    assert_eq!(
        library.parent().unwrap().file_name(),
        executable.parent().unwrap().file_name()
    );
    run(&executable, 42, "");

    fs::write(api.join("src/offsets.co"), "pub fn<int> offset() = 3;\n").unwrap();
    run(&artifact(&mut build(&app)), 42, "");
    artifact(build(&api).arg("arithmetic-lib"));
    run(&artifact(&mut build(&app)), 43, "");
    fs::write(api.join("src/offsets.co"), "pub fn<int> offset() = 4;\n").unwrap();
    write(&app, "build.k", &consumer(false));
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
        app.join("src/main.co"),
        "using Arithmetic : adjust;\npub fn<int> main() = adjust(40);\n",
    )
    .unwrap();
    failure(build(&app).output().unwrap(), "'adjust' is private");
    run(&executable, 44, "");
}

#[test]
fn collections_project_links_the_packaged_runtime() {
    let work = Directory::new("collections");
    write(&work.0, "src/main.co", COLLECTIONS);
    write(
        &work.0,
        "build.k",
        r#"project "collections" {
        target "executable" entry="src/main.co"
        output "out"
    }"#,
    );
    let stdlib = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("Stdlib/StandardLibrary/src");
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
    for (root, name, other, path) in [
        (work.0.clone(), "Left", "Right", "right"),
        (work.0.join("right"), "Right", "Left", ".."),
    ] {
        write(&root, "src/api.co", "pub fn<int> value() = 7;\n");
        write(
            &root,
            "build.k",
            &format!(
                r#"project "{name}" {{
            dependency "{other}" path="{path}"
            target "library" entry="src/api.co" {{
                uses "{other}"
            }}
        }}"#
            ),
        );
    }
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
        ("Left", "pub fn<int> value() = 40;\n", None),
        ("Right", "pub fn<int> value() = 2;\n", None),
        (
            "First",
            "using Left : value;\npub fn<int> first() = value();\n",
            Some("Left"),
        ),
        (
            "Second",
            "using Right : value;\npub fn<int> second() = value();\n",
            Some("Right"),
        ),
    ];
    for (name, source, dependency) in cases {
        let dir = work.0.join(name);
        write(&dir, "src/api.co", source);
        let mut manifest = format!("project \"{name}\" {{\n");
        if let Some(other) = dependency {
            manifest += &format!("dependency \"{other}\" path=\"../{other}\"\n");
            manifest +=
                &format!("target \"library\" entry=\"src/api.co\" {{\nuses \"{other}\"\n}}\n");
        } else {
            manifest += "target \"library\" entry=\"src/api.co\"\n";
        }
        write(&dir, "build.k", &(manifest + "}\n"));
    }
    write(
        &work.0,
        "src/main.co",
        "using First : first;\nusing Second : second;\npub fn<int> main() = first() + second();\n",
    );
    write(
        &work.0,
        "build.k",
        r#"project "sum" {
        dependency "First" path="First/build.k"
        dependency "Second" path="Second"
        target "executable" entry="src/main.co" {
            uses "First"
            uses "Second"
        }
    }"#,
    );
    run(&artifact(&mut build(&work.0)), 42, "");
}

#[test]
fn sibling_modules_supply_an_executable_and_a_named_library() {
    let work = Directory::new("two modules");
    let root = work.0.join("calculator");
    write(
        &root,
        "build.k",
        r#"project "calculator" version="0.1.0" {
        include "Console"
        include "Arithmetic"
        target "executable" entry="Console/src/main.co"
        target "library" name="arithmetic-lib" module="Arithmetic"
    }"#,
    );
    write(
        &root,
        "Console/module.k",
        "module \"Console\" {\nuses \"Arithmetic\"\n}",
    );
    write(&root, "Console/src/main.co", MAIN);
    write(
        &root,
        "Arithmetic/module.k",
        "module \"Arithmetic\" api=\"src/api.co\"",
    );
    write(&root, "Arithmetic/src/api.co", API);
    write(&root, "Arithmetic/src/offsets.co", OFFSETS);

    let executable = artifact(&mut build(&root));
    assert_eq!(executable.file_stem().unwrap(), "calculator");
    run(&executable, 42, "");
    let library =
        artifact(build(&work.0).args(["arithmetic-lib", "--manifest-path", "calculator/build.k"]));
    assert_eq!(library.file_name().unwrap(), "arithmetic-lib.o");

    write(
        &root,
        "build.k",
        "project \"calculator\" {\ninclude \"Arithmetic\"\ntarget \"library\" module=\"Arithmetic\"\n}",
    );
    assert_eq!(
        artifact(&mut build(&root)).file_name().unwrap(),
        "calculator.o"
    );
}

#[test]
fn root_sources_can_add_targets_and_import_a_local_module() {
    let work = Directory::new("root sources");
    write(&work.0, "src/main.co", "pub fn<int> main() = 7;\n");
    write(
        &work.0,
        "build.k",
        "project \"hello\" {\ntarget \"executable\" entry=\"src/main.co\"\n}",
    );
    run(&artifact(&mut build(&work.0)), 7, "");

    write(
        &work.0,
        "Arithmetic/module.k",
        "module \"Arithmetic\" api=\"src/api.co\"",
    );
    write(&work.0, "Arithmetic/src/api.co", API);
    write(&work.0, "Arithmetic/src/offsets.co", OFFSETS);
    write(&work.0, "src/inspect.co", MAIN);
    write(
        &work.0,
        "build.k",
        r#"project "hello" {
        include "Arithmetic"
        target "executable" entry="src/main.co"
        target "executable" name="inspect" entry="src/inspect.co" {
            uses "Arithmetic"
        }
    }"#,
    );
    run(&artifact(&mut build(&work.0)), 7, "");
    run(&artifact(build(&work.0).arg("inspect")), 42, "");
}

#[test]
fn manifest_errors_identify_the_declaration_before_building() {
    let work = Directory::new("manifest diagnostics");
    write(&work.0, "src/main.co", "pub fn<int> main() = 0;\n");
    write(&work.0, "outside.co", "pub fn<int> main() = 1;\n");
    let cases = [
        (
            "targt \"executable\"",
            "build.k:2:5: unknown project node 'targt'",
        ),
        (
            "target \"object\" entry=\"src/main.co\"",
            "build.k:2:5: unknown target kind 'object'",
        ),
        (
            "target \"executable\" entry=\"src/main.co\" entry=\"outside.co\"",
            "duplicate property 'entry'",
        ),
        (
            "target \"executable\" entry=\"src/main.co\"\n    target \"library\" entry=\"src/main.co\"",
            "duplicate target name 'test'",
        ),
        (
            "target \"executable\" entry=\"src/../outside.co\"",
            "outside the project and included module src directories",
        ),
        (
            "target \"executable\" entry=\"src/main.co\" {\nuses \"Missing\"\n}",
            "unknown module 'Missing'",
        ),
        (
            "target \"executable\" entry=\"src/main.co\"\ninclude \"Missing\"",
            "module.k",
        ),
        (
            "target \"library\" name=\"one\" module=\"One\" entry=\"src/main.co\"\ntarget \"library\" name=\"two\" module=\"Two\" entry=\"src/main.co\"",
            "available targets: one, two",
        ),
        ("target \"executable\" entry=\"unterminated", "build.k:2:"),
        (
            "dependency \"Local\" path=\"../provider\"\ntarget \"library\" module=\"Local\" entry=\"src/main.co\"",
            "local module and dependency both supply 'Local'",
        ),
    ];
    for (body, expected) in cases {
        write(
            &work.0,
            "build.k",
            &format!("project \"test\" {{\n    {body}\n}}\n"),
        );
        failure(build(&work.0).output().unwrap(), expected);
        assert!(!work.0.join("build").exists());
    }
    for folder in ["First", "Second"] {
        write(
            &work.0,
            &format!("{folder}/module.k"),
            "module \"Repeated\"",
        );
        write(
            &work.0,
            &format!("{folder}/src/api.co"),
            "pub fn<int> value() = 0;\n",
        );
    }
    write(
        &work.0,
        "build.k",
        "project \"test\" {\ninclude \"First\"\ninclude \"Second\"\ntarget \"executable\" entry=\"src/main.co\"\n}",
    );
    failure(
        build(&work.0).output().unwrap(),
        "duplicate module 'Repeated'",
    );
    write(
        &work.0,
        "First/module.k",
        "module \"First\" {\u{2028}    uses \"Missing\"\r\n}",
    );
    write(
        &work.0,
        "build.k",
        "project \"test\" {\ninclude \"First\"\ntarget \"executable\" entry=\"First/src/api.co\"\n}",
    );
    failure(
        build(&work.0).output().unwrap(),
        "module.k:2:5: unknown module 'Missing'",
    );
    let (name, second_target) = if cfg!(windows) {
        (
            "sample",
            r#"target "executable" name="SAMPLE" entry="src/main.co""#,
        )
    } else {
        (
            "sample.o",
            r#"target "library" name="sample" module="Sample" entry="src/main.co""#,
        )
    };
    write(
        &work.0,
        "build.k",
        &format!(
            "project \"{name}\" {{\ntarget \"executable\" entry=\"src/main.co\"\n{second_target}\n}}"
        ),
    );
    failure(build(&work.0).output().unwrap(), "same output filename");
}
