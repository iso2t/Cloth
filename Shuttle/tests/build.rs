// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use std::env;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};

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

fn init(dir: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_shuttle"))
        .current_dir(dir)
        .arg("init")
        .args(args)
        .output()
        .unwrap()
}

fn success(output: Output) {
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

fn compiler_path() -> PathBuf {
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    env::var_os("CLOTHC").map(PathBuf::from).unwrap_or_else(|| {
        repo.join("build")
            .join(if cfg!(windows) { "cloth.exe" } else { "cloth" })
    })
}

fn shuttle(dir: &Path, subcommand: &str) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_shuttle"));
    command
        .current_dir(dir)
        .arg(subcommand)
        .arg("--compiler")
        .arg(compiler_path());
    if let Ok(bits) = env::var("SHUTTLE_TEST_TARGET_BITS") {
        command.args(["--target-bits", &bits]);
    }
    command
}

fn build(dir: &Path) -> Command {
    shuttle(dir, "build")
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
    assert_program(Command::new(path).output().unwrap(), code, stdout);
}

fn assert_program(output: Output, code: i32, stdout: &str) {
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
fn missing_command_shows_the_same_guide_as_the_help_flags() {
    let shuttle = |args: &[&str]| {
        Command::new(env!("CARGO_BIN_EXE_shuttle"))
            .args(args)
            .output()
            .unwrap()
    };
    let requested = shuttle(&["--help"]);
    assert!(requested.status.success());
    assert!(requested.stderr.is_empty());
    let help = std::str::from_utf8(&requested.stdout).unwrap();
    for heading in ["Usage", "Examples", "Project options", "Toolchain options"] {
        assert!(help.lines().any(|line| line == heading), "{help}");
    }
    assert!(help.contains("shuttle build arithmetic-lib"));
    assert!(help.contains("shuttle init --module NAME"));
    assert!(help.contains("shuttle run [PATH]"));

    let missing = shuttle(&[]);
    assert!(missing.stdout.is_empty());
    failure(missing, help);

    for args in [
        &["-h"][..],
        &["init", "--help"],
        &["run", "--help"],
        &["build", "--help"],
        &["build", "arithmetic-lib", "-h"],
    ] {
        let output = shuttle(args);
        assert!(output.status.success());
        assert!(output.stderr.is_empty());
        assert_eq!(output.stdout, requested.stdout);
    }
}

#[test]
fn init_creates_a_runnable_project_and_an_importable_module() {
    let work = Directory::new("init");
    let root = work.0.join("hello π");
    success(init(&work.0, &["hello π"]));
    run(&artifact(&mut build(&root)), 0, "Hello, world!\n");

    success(init(&root.join("src"), &["--module", "Arithmetic"]));
    let manifest = fs::read_to_string(root.join("build.k")).unwrap();
    assert!(manifest.contains("include \"Arithmetic\""));
    let consumer = manifest.replace(
        "entry=\"src/main.co\"",
        "entry=\"src/main.co\" {\n        uses \"Arithmetic\"\n    }",
    );
    write(&root, "build.k", &consumer);
    write(
        &root,
        "src/main.co",
        "using Arithmetic : answer;\npub fn<int> main() = answer();\n",
    );
    run(&artifact(&mut build(&root)), 42, "");
}

#[test]
fn init_preserves_existing_files_and_requires_a_project_for_modules() {
    let work = Directory::new("init existing");
    failure(init(&work.0, &["--module", "Math"]), "no build.k");
    failure(init(&work.0, &["--module"]), "requires a module name");
    failure(init(&work.0, &["one", "two"]), "accepts one");
    failure(init(&work.0, &["--unknown"]), "unexpected init option");
    assert_eq!(fs::read_dir(&work.0).unwrap().count(), 0);

    write(&work.0, "src/main.co", "user source");
    failure(init(&work.0, &[]), "main.co");
    assert_eq!(
        fs::read_to_string(work.0.join("src/main.co")).unwrap(),
        "user source"
    );
    assert!(!work.0.join("build.k").exists());

    let app = work.0.join("existing app");
    write(&app, "src/other.co", "user source");
    success(init(&app, &[]));
    assert_eq!(
        fs::read_to_string(app.join("src/other.co")).unwrap(),
        "user source"
    );
    let manifest = fs::read_to_string(app.join("build.k")).unwrap();
    failure(init(&app, &[]), "build.k already exists");
    for name in ["bad-name", "../escape", "fn", "_"] {
        failure(
            init(&app, &["--module", name]),
            "must be a Cloth identifier",
        );
    }
    write(&app, "Existing/module.k", "user manifest");
    failure(init(&app, &["--module", "Existing"]), "Existing");
    assert_eq!(
        fs::read_to_string(app.join("Existing/module.k")).unwrap(),
        "user manifest"
    );
    assert_eq!(fs::read_to_string(app.join("build.k")).unwrap(), manifest);
    assert!(!work.0.join("escape").exists());

    let invalid = "project \"broken\" { unknown \"value\" }\n";
    write(&app, "build.k", invalid);
    failure(init(&app, &["--module", "Math"]), "unknown project node");
    assert_eq!(fs::read_to_string(app.join("build.k")).unwrap(), invalid);
    assert!(!app.join("Math").exists());
}

#[test]
fn module_init_preserves_kdl_and_checks_existing_providers() {
    let work = Directory::new("module registration");
    let root = &work.0;
    write(root, "src/api.co", "pub fn<int> value() = 0;\n");
    write(
        root,
        "Alias/module.k",
        "module \"Café\" api=\"src/api.co\"\n",
    );
    write(root, "Alias/src/api.co", "pub fn<int> value() = 1;\n");
    let before = concat!(
        "// Project {header}\r\nproject tools {\r\n",
        "\t// Keep this comment with the first target.\r\n",
        "\ttarget library name=\"first\" module=\"Exported\" entry=\"src/api.co\"\r\n",
        "\ttarget library name=\"second\" module=\"Other\" entry=\"src/api.co\"\r\n",
        "\tdependency \"Foreign\" path=\"../foreign\"\r\n",
        "\tinclude \"Alias\"\r\n}\r\n// trailing }\r\n",
    );
    write(root, "build.k", before);
    for name in ["Cafe\u{301}", "Exported", "Foreign"] {
        failure(init(root, &["--module", name]), "already in use");
        assert!(!root.join(name).exists());
    }
    success(init(root, &["--module", "Math"]));
    let registered = before.replacen("tools {\r\n", "tools {\r\n\tinclude \"Math\"\r\n", 1);
    assert_eq!(
        fs::read_to_string(root.join("build.k")).unwrap(),
        registered
    );
    write(root, "Math/src/api.co", "user edits");
    failure(init(root, &["--module", "Math"]), "already in use");
    assert_eq!(
        fs::read_to_string(root.join("Math/src/api.co")).unwrap(),
        "user edits"
    );
    assert_eq!(
        fs::read_to_string(root.join("build.k")).unwrap(),
        registered
    );

    let inline = "project tools { target library module=\"Other\" entry=\"src/api.co\" } // }";
    write(root, "build.k", inline);
    success(init(root, &["--module", "Ω"]));
    let registered = fs::read_to_string(root.join("build.k")).unwrap();
    assert_eq!(registered, inline.replacen("{", "{ include \"Ω\";", 1));
    artifact(&mut build(root));
}

#[test]
fn concurrent_module_additions_preserve_each_registration() {
    let work = Directory::new("concurrent init");
    let root = &work.0;
    success(init(root, &[]));
    let names = ["Left", "Right"];
    let results = std::thread::scope(|scope| {
        let jobs = names.map(|name| scope.spawn(move || init(root, &["--module", name])));
        jobs.map(|job| job.join().unwrap())
    });
    for (name, output) in names.into_iter().zip(results) {
        if !output.status.success() {
            failure(output, ".build.k.shuttle-init");
            success(init(root, &["--module", name]));
        }
    }
    let manifest = fs::read_to_string(root.join("build.k")).unwrap();
    for name in names {
        assert_eq!(manifest.matches(&format!("include \"{name}\"")).count(), 1);
        assert!(root.join(name).join("module.k").is_file());
    }
}

#[test]
fn run_builds_the_whole_project_and_forwards_arguments_and_exit_status() {
    let work = Directory::new("run");
    let root = work.0.join("hello π");
    success(init(&work.0, &["hello π"]));
    assert_program(
        shuttle(&root, "run").output().unwrap(),
        0,
        "Hello, world!\n",
    );
    for module in ["Arithmetic", "Unused"] {
        success(init(&root, &["--module", module]));
    }
    write(
        &root,
        "build.k",
        r#"project "hello π" {
        include "Arithmetic"
        include "Unused"
        target "executable" entry="src/main.co" { uses "Arithmetic" }
        target "library" name="arithmetic-lib" module="Arithmetic"
        target "executable" name="inspect" entry="src/inspect.co"
        output "out"
    }"#,
    );
    write(&root, "asset.txt", "project-relative input");
    write(&root, "src/inspect.co", "pub fn<int> main() = 7;\n");
    write(
        &root,
        "src/main.co",
        r#"using Arithmetic : answer;
using cloth.Console : println;
using cloth.io.fs.File : File;
pub fn<int> main(string[] args) {
    def file = File::new("asset.txt");
    if (file.open() != null) return 5;
    file::delete;
    if (args::size != 3) return 6;
    println(args[0]);
    println(args[1]);
    println(args[2]);
    return answer();
}
"#,
    );
    let args = ["--", "two words π", "--help", ""];
    let output = shuttle(&root.join("src"), "run")
        .args(args)
        .output()
        .unwrap();
    assert_program(output, 42, "two words π\n--help\n\n");
    let triple = fs::read_dir(root.join("out"))
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .find(|path| path.is_dir())
        .unwrap();
    assert!(triple.join("arithmetic-lib.o").is_file());
    let inspect = triple.join(format!("inspect{}", env::consts::EXE_SUFFIX));
    run(&inspect, 7, "");

    write(&root, "src/inspect.co", "pub fn<int> main() = 8;\n");
    write(
        &root,
        "Unused/src/api.co",
        "pub fn<int> broken() = missing();\n",
    );
    let failed = shuttle(&work.0, "run")
        .arg("hello π")
        .args(args)
        .output()
        .unwrap();
    assert!(failed.stdout.is_empty());
    failure(failed, "missing");
    run(&inspect, 7, "");
}

#[test]
fn failed_publication_preserves_every_previous_output() {
    let work = Directory::new("publication");
    let root = &work.0;
    let manifest = r#"project app {
        target executable entry="src/main.co"
        target library name="api" module="Api" entry="src/api.co"
        target executable name="tool" entry="src/tool.co"
    }"#;
    write(root, "build.k", manifest);
    write(root, "src/main.co", "pub fn<int> main() = 7;");
    write(root, "src/api.co", "pub fn<int> value() = 10;");
    write(root, "src/tool.co", "pub fn<int> main() = 11;");
    let app = artifact(&mut build(root));
    assert_program(shuttle(root, "run").output().unwrap(), 7, "");
    let directory = app.parent().unwrap();
    let executable = |name: &str| directory.join(format!("{name}{}", env::consts::EXE_SUFFIX));
    let library = directory.join("api.o");
    let tool = executable("tool");
    let outputs = [&app, &library, &tool];
    let previous = outputs.map(|path| fs::read(path).unwrap());

    write(root, "src/main.co", "pub fn<int> main() = 42;");
    write(root, "src/api.co", "pub fn<int> value() = 20;");
    write(root, "src/tool.co", "pub fn<int> main() = 21;");
    write(
        root,
        "build.k",
        &manifest.replace(
            '}',
            r#"
        target executable name="new" entry="src/main.co"
        target executable name="zz-blocked" entry="src/tool.co"
    }"#,
        ),
    );
    let blocked = executable("zz-blocked");
    fs::create_dir(&blocked).unwrap();
    let sentinel = blocked.join("keep");
    fs::write(&sentinel, "original").unwrap();

    let assert_previous = || {
        for (path, old) in outputs.into_iter().zip(&previous) {
            assert!(
                fs::read(path).unwrap() == *old,
                "{} changed",
                path.display()
            );
        }
        assert!(!executable("new").exists());
    };
    let failed = shuttle(root, "run").output().unwrap();
    assert!(failed.stdout.is_empty());
    failure(failed, "zz-blocked");
    assert_previous();
    assert_eq!(fs::read(&sentinel).unwrap(), b"original");
    run(&app, 7, "");
    run(&tool, 11, "");

    fs::remove_file(sentinel).unwrap();
    fs::remove_dir(&blocked).unwrap();
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        // Reading is allowed, but Windows will refuse to replace this executable.
        let _held = fs::File::options()
            .read(true)
            .share_mode(1)
            .open(&tool)
            .unwrap();
        failure(shuttle(root, "run").output().unwrap(), "tool.exe");
        assert_previous();
        assert!(!blocked.exists());
    }
    assert_program(shuttle(root, "run").output().unwrap(), 42, "");
    assert!(fs::read(&library).unwrap() != previous[1]);
    run(&executable("new"), 42, "");
    run(&blocked, 21, "");
    run(&tool, 21, "");
}

#[test]
fn run_selects_only_executables_and_inherits_standard_input() {
    let work = Directory::new("run targets");
    let app = work.0.join("app");
    failure(shuttle(&work.0, "run").output().unwrap(), "no build.k");
    write(&app, "src/api.co", "pub fn<int> value() = 1;\n");
    let library = "target library entry=\"src/api.co\"";
    let echo = "target executable name=\"echo\" entry=\"src/echo.co\"";
    let other = "target executable name=\"other\" entry=\"src/other.co\"";
    write(&app, "build.k", &format!("project tools {{\n{library}\n}}"));
    failure(
        shuttle(&app, "run").output().unwrap(),
        "no executable targets",
    );
    write(
        &app,
        "src/echo.co",
        "using cloth.Console : input, println;\npub fn<int> main() {\n    println(input());\n    return 19;\n}\n",
    );
    write(&app, "src/other.co", "pub fn<int> main() = 23;\n");
    write(
        &app,
        "build.k",
        &format!("project tools {{\n{library}\n{echo}\n{other}\n}}"),
    );
    failure(
        shuttle(&app, "run").output().unwrap(),
        "available executables: echo, other",
    );
    failure(
        shuttle(&app, "run")
            .args(["--target", "tools"])
            .output()
            .unwrap(),
        "is a library",
    );
    failure(
        shuttle(&app, "run")
            .args(["--target", "unknown"])
            .output()
            .unwrap(),
        "unknown executable",
    );
    assert!(!app.join("build").exists());
    assert_program(
        shuttle(&work.0, "run")
            .args(["app/src", "--target", "other"])
            .output()
            .unwrap(),
        23,
        "",
    );

    write(
        &app,
        "build.k",
        &format!("project tools {{\n{library}\n{echo}\n}}"),
    );
    let mut child = shuttle(&work.0, "run")
        .args(["--manifest-path", "app/build.k"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    child
        .stdin
        .take()
        .unwrap()
        .write_all(b"from stdin\n")
        .unwrap();
    assert_program(child.wait_with_output().unwrap(), 19, "from stdin\n");
}

#[test]
fn whole_project_runs_keep_each_executables_library_bindings() {
    let work = Directory::new("run providers");
    let root = &work.0;
    let api = root.join("api");
    let library = "project Shared { target library entry=\"src/api.co\" }";
    write(&api, "build.k", library);
    write(&api, "src/api.co", "pub fn<int> value() = 10;\n");
    let prebuilt = artifact(&mut build(&api));
    write(&api, "src/api.co", "pub fn<int> value() = 20;\n");
    let wrapper = |name: &str, provider: &str, prebuilt: bool| {
        format!(
            r#"project "{name}" {{
        dependency "Shared" path="{provider}" prebuilt=#{prebuilt}
        target library entry="src/api.co" {{ uses "Shared" }}
    }}"#
        )
    };
    for (name, prebuilt) in [("Left", true), ("Right", false)] {
        write(
            &root.join(name),
            "build.k",
            &wrapper(name, "../api", prebuilt),
        );
        write(
            &root.join(name),
            "src/api.co",
            &format!("using Shared : value;\npub fn<int> from{name}() = value();\n"),
        );
        write(
            root,
            &format!("src/{name}.co"),
            &format!("using {name} : from{name};\npub fn<int> main() = from{name}();\n"),
        );
    }
    let manifest = r#"project app {
        dependency "Left" path="Left"
        dependency "Right" path="Right"
        target executable entry="src/Left.co" { uses "Left" }
        target executable name="second" entry="src/Right.co" { uses "Right" }
    }"#;
    write(root, "build.k", manifest);
    assert_program(shuttle(root, "run").output().unwrap(), 10, "");
    let second = root
        .join("build")
        .join(prebuilt.parent().unwrap().file_name().unwrap())
        .join(format!("second{}", env::consts::EXE_SUFFIX));
    run(&second, 20, "");

    let v2 = root.join("v2");
    write(&v2, "build.k", library);
    write(&v2, "src/api.co", "pub fn<int> value() = 30;\n");
    write(
        &root.join("Right"),
        "build.k",
        &wrapper("Right", "../v2", false),
    );
    assert_program(
        shuttle(root, "run")
            .args(["--target", "second"])
            .output()
            .unwrap(),
        30,
        "",
    );
    write(
        root,
        "build.k",
        &manifest.replace("uses \"Left\"", "uses \"Left\"; uses \"Right\""),
    );
    failure(
        shuttle(root, "run").output().unwrap(),
        "module 'Shared' is supplied by",
    );
}

#[test]
fn prebuilt_library_must_match_the_named_argument_interface() {
    let work = Directory::new("prebuilt API π");
    let api = work.0.join("provider");
    let app = work.0.join("consumer");
    write(
        &api,
        "build.k",
        r#"project "calculator" { target "library" entry="src/api.co" name="math-lib" module="Arithmetic" }"#,
    );
    let signature = "pub fn<int> diff(int left, int right) = left - right;\n";
    let overload = "pub fn<int> diff(bool flag) = 0;\n";
    write(&api, "src/api.co", &format!("{signature}{overload}"));
    write(
        &app,
        "build.k",
        r#"project "answer" {
            dependency "Arithmetic" path="../provider" prebuilt=#true
            target "executable" entry="src/main.co" { uses "Arithmetic" }
        }"#,
    );
    write(
        &app,
        "src/main.co",
        "using Arithmetic : diff;\nusing cloth.Console : println;\n\
         pub fn<void> main() { println(diff(left=43, right=1)); }\n",
    );
    let library = artifact(&mut build(&api));
    let executable = artifact(&mut build(&app));
    run(&executable, 0, "42\n");

    let body_edit = signature.replace("left - right", "left + right");
    write(&api, "src/api.co", &format!("{overload}{body_edit}"));
    assert_program(shuttle(&app, "run").output().unwrap(), 0, "42\n");
    let previous = fs::read(&executable).unwrap();

    // The caller supplies both arguments, so the native linker alone would accept either object.
    let default_added = signature.replace("int right)", "opt<int> right ?: 1)");
    write(&api, "src/api.co", &format!("{overload}{default_added}"));
    failure(
        build(&app).output().unwrap(),
        "rebuild prebuilt library 'Arithmetic'",
    );
    let swapped = signature.replace("left, int right", "right, int left");
    write(&api, "src/api.co", &format!("{overload}{swapped}"));
    failure(
        shuttle(&app, "run").output().unwrap(),
        "rebuild prebuilt library 'Arithmetic' (target 'math-lib'",
    );
    assert_eq!(fs::read(&executable).unwrap(), previous);
    run(&executable, 0, "42\n");

    artifact(&mut build(&api));
    assert_program(shuttle(&app, "run").output().unwrap(), 0, "42\n");

    // A native object without an interface record must also fail before linking.
    write(&api, "src/plain.co", "pub fn<int> main() = 0;\n");
    let mut cloth = Command::new(compiler_path());
    if let Ok(bits) = env::var("SHUTTLE_TEST_TARGET_BITS") {
        cloth.args(["--target-bits", &bits]);
    }
    success(
        cloth
            .current_dir(&api)
            .args([
                "--src",
                "src/plain.co",
                "--kind",
                "executable",
                "--emit-object",
            ])
            .arg(&library)
            .output()
            .unwrap(),
    );
    let rejected = build(&app).output().unwrap();
    assert!(
        String::from_utf8_lossy(&rejected.stderr).contains("object has no Cloth interface record")
    );
    failure(rejected, "rebuild prebuilt library 'Arithmetic'");
    run(&executable, 0, "42\n");
}

#[test]
fn prebuilt_library_must_match_the_returned_struct_layout() {
    let work = Directory::new("prebuilt struct");
    let api = work.0.join("provider");
    let app = work.0.join("consumer");
    write(
        &api,
        "build.k",
        r#"project geometry { target library entry="src/api.co" module="Geometry" }"#,
    );
    let point_api = |fields: &str, first: i32| {
        write(
            &api,
            "src/api.co",
            &format!(
                "pub typedef<struct> Point {{ {fields}; }}\npub fn<Point> origin() = {{{first}, 7}};\n"
            ),
        );
    };
    point_api("pub int x, pub int y", 42);
    write(
        &app,
        "build.k",
        r#"project picture {
            dependency Geometry path="../provider" prebuilt=#true
            target executable entry="src/main.co" { uses Geometry }
        }"#,
    );
    write(
        &app,
        "src/main.co",
        "using Geometry : origin;\npub fn<int> main() = origin().x;\n",
    );
    artifact(&mut build(&api));
    let executable = artifact(&mut build(&app));
    run(&executable, 42, "");

    // Equivalent field types let the consumer link the existing provider and receive its original value.
    point_api("pub int<32> x, pub int y", 99);
    run(&artifact(&mut build(&app)), 42, "");

    // Each changed API still type-checks the caller. Swapping x and y even preserves size and alignment.
    for fields in [
        "pub int x, pub int<64> y",
        "pub int x, pub unsigned int y",
        "pub int y, pub int x",
    ] {
        point_api(fields, 42);
        let rejected = build(&app).output().unwrap();
        assert!(
            String::from_utf8_lossy(&rejected.stderr)
                .contains("native interface for 'Geometry' differs from the checked source")
        );
        failure(rejected, "rebuild prebuilt library 'Geometry'");
    }
    run(&executable, 42, "");
    artifact(&mut build(&api));
    run(&artifact(&mut build(&app)), 7, "");
}

#[test]
fn prebuilt_library_keeps_the_interfaces_of_its_native_dependencies() {
    let work = Directory::new("prebuilt dependency");
    let api = work.0.join("arithmetic");
    let adapter = work.0.join("adapter");
    let app = work.0.join("app");
    write(
        &api,
        "build.k",
        r#"project Arithmetic { target library entry="src/api.co" }"#,
    );
    let signature = "pub fn<int> diff(int left, int right) = left - right;\n";
    write(&api, "src/api.co", signature);
    write(
        &adapter,
        "build.k",
        r#"project Adapter {
        dependency Arithmetic path="../arithmetic"
        target library entry="src/api.co" { uses Arithmetic }
    }"#,
    );
    write(
        &adapter,
        "src/api.co",
        "using Arithmetic : diff;\npub fn<int> answer() = diff(left=43, right=1);\n",
    );
    write(
        &app,
        "build.k",
        r#"project app {
        dependency Adapter path="../adapter" prebuilt=#true
        target executable entry="src/main.co" { uses Adapter }
    }"#,
    );
    write(
        &app,
        "src/main.co",
        "using Adapter : answer;\npub fn<int> main() = answer();\n",
    );
    artifact(&mut build(&adapter));
    let executable = artifact(&mut build(&app));
    run(&executable, 42, "");

    // Only the old Adapter object still calls Arithmetic.
    write(&adapter, "src/api.co", "pub fn<int> answer() = 42;\n");
    write(
        &api,
        "src/api.co",
        &signature.replace("left, int right", "right, int left"),
    );
    let rejected = build(&app).output().unwrap();
    assert!(
        String::from_utf8_lossy(&rejected.stderr)
            .contains("native interface for 'Arithmetic' differs")
    );
    failure(rejected, "rebuild prebuilt library 'Adapter'");
    run(&executable, 42, "");
    artifact(&mut build(&adapter));
    run(&artifact(&mut build(&app)), 42, "");
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
