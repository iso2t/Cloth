// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Shuttle/README.rst

use crate::staging::{Staging, lock_directory};
use crate::{HELP, Result, manifest, project};
use kdl::KdlDocument;
use std::ffi::OsString;
use std::fs::{self, File};
use std::io::{self, ErrorKind, Write};
use std::path::{Path, PathBuf};

const MAIN: &str =
    "using cloth.Console : println;\n\npub fn main() {\n    println(\"Hello, world!\");\n}\n";
const API: &str = "pub fn<int> answer() = 42;\n";
const PENDING_INCLUDE: &str = ".shuttle-pending-include.k";

pub fn run(args: impl Iterator<Item = OsString>) -> Result<()> {
    let mut module = false;
    let mut destination = None;
    for arg in args {
        match arg.to_str() {
            Some("-h" | "--help") => {
                println!("{HELP}");
                return Ok(());
            }
            Some("--module") if !module => module = true,
            Some(flag) if flag.starts_with('-') => {
                return Err(format!("unexpected init option '{flag}'").into());
            }
            _ => {
                if destination.replace(arg).is_some() {
                    return Err("shuttle init accepts one project path or module name".into());
                }
            }
        }
    }
    if module {
        let name = destination
            .as_deref()
            .ok_or("--module requires a module name")?;
        create_module(name.to_str().ok_or("module names must be UTF-8")?)
    } else {
        create_project(Path::new(&destination.unwrap_or_else(|| ".".into())))
    }
}

// Roll back only paths created by this invocation, leaving existing directories
// and any files added by another process in place.
#[derive(Default)]
struct Created {
    files: Vec<PathBuf>,
    directories: Vec<PathBuf>,
}

impl Created {
    fn directory(&mut self, path: &Path) -> Result<()> {
        fs::create_dir(path).map_err(|e| format!("creating {}: {e}", path.display()))?;
        self.directories.push(path.to_owned());
        Ok(())
    }

    fn ensure_directory(&mut self, path: &Path) -> Result<()> {
        if path.is_dir() {
            return Ok(());
        }
        if let Some(parent) = path.parent() {
            self.ensure_directory(parent)?;
        }
        self.directory(path)
    }

    fn file(&mut self, path: &Path) -> Result<File> {
        let file =
            File::create_new(path).map_err(|e| format!("creating {}: {e}", path.display()))?;
        self.files.push(path.to_owned());
        Ok(file)
    }

    fn write(&mut self, path: &Path, contents: &str) -> Result<()> {
        self.file(path)?.write_all(contents.as_bytes())?;
        Ok(())
    }

    fn commit(mut self) {
        self.files.clear();
        self.directories.clear();
    }
}

impl Drop for Created {
    fn drop(&mut self) {
        for path in self.files.iter().rev() {
            let _ = fs::remove_file(path);
        }
        for path in self.directories.iter().rev() {
            let _ = fs::remove_dir(path);
        }
    }
}

fn create_project(path: &Path) -> Result<()> {
    let mut created = Created::default();
    let directory = std::path::absolute(path)?;
    created.ensure_directory(&directory)?;
    let directory = dunce::canonicalize(directory)?;
    let name = directory
        .file_name()
        .and_then(|n| n.to_str())
        .filter(|n| manifest::filename(n))
        .ok_or_else(|| {
            format!(
                "{} needs a valid project directory name",
                directory.display()
            )
        })?;
    let manifest = directory.join("build.k");
    if manifest.try_exists()? {
        return Err(format!("{} already exists", manifest.display()).into());
    }
    let src = directory.join("src");
    match fs::symlink_metadata(&src) {
        Ok(entry) if entry.is_dir() => {}
        Ok(_) => return Err(format!("{} must be a project directory", src.display()).into()),
        Err(e) if e.kind() == ErrorKind::NotFound => created.directory(&src)?,
        Err(e) => return Err(e.into()),
    }
    created.write(&src.join("main.co"), MAIN)?;
    created.write(&manifest, &format!(
        "project {name:?} version=\"0.1.0\" {{\n    target \"executable\" entry=\"src/main.co\"\n}}\n"
    ))?;
    created.commit();
    println!("Created project '{name}' in {}", directory.display());
    Ok(())
}

fn with_include(text: &str, name: &str) -> Result<String> {
    let doc = KdlDocument::parse_v2(text)?;
    let first = doc
        .get("project")
        .and_then(|p| p.iter_children().next())
        .ok_or("project requires at least one target")?;
    // Insert before the first child's leading comments. Leave the original
    // manifest bytes intact, including inline nodes and the file's line endings.
    let leading = first.format().map_or("", |f| f.leading.as_str());
    let offset = first.span().offset() - leading.len();
    let addition = if let Some(eol) = ["\r\n", "\n", "\r"]
        .into_iter()
        .find(|eol| leading.starts_with(eol))
    {
        let indent = leading.rsplit(['\r', '\n']).next().unwrap_or("");
        let indent = if indent.trim().is_empty() {
            indent
        } else {
            "    "
        };
        format!("{eol}{indent}include \"{name}\"")
    } else {
        format!(" include \"{name}\";")
    };
    let mut updated = text.to_owned();
    updated.insert_str(offset, &addition);
    Ok(updated)
}

fn existing_entry(path: &Path) -> io::Result<Option<fs::Metadata>> {
    match fs::symlink_metadata(path) {
        Ok(entry) => Ok(Some(entry)),
        Err(e) if e.kind() == ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

// with_include inserts the requested module as the first child. The live
// manifest may have acquired other includes since this draft was written.
fn draft_include(draft: &[u8]) -> Option<(String, String)> {
    let doc = KdlDocument::parse_v2(std::str::from_utf8(draft).ok()?).ok()?;
    let project = doc.get("project")?;
    let first = project.iter_children().next()?;
    if first.name().value() != "include" {
        return None;
    }
    Some((
        project.entry(0)?.value().as_string()?.to_owned(),
        first.entry(0)?.value().as_string()?.to_owned(),
    ))
}

fn finish_legacy_scaffold(directory: &Path, module_text: &str, staging: &Path) -> Result<()> {
    let src = directory.join("src");
    let src_entry = existing_entry(&src)?;
    if src_entry.as_ref().is_some_and(|m| !m.is_dir()) {
        return Err(format!("{} must be a directory", src.display()).into());
    }
    // Check both files before completing either. A stopped write can leave a
    // prefix of the template. Other contents belong to the user.
    let mut repairs = Vec::new();
    for (path, text) in [
        (src.join("api.co"), API),
        (directory.join("module.k"), module_text),
    ] {
        let entry = existing_entry(&path)?;
        if let Some(meta) = &entry {
            if !meta.is_file()
                || meta.len() > text.len() as u64
                || !text.as_bytes().starts_with(&fs::read(&path)?)
            {
                return Err(
                    format!("{} conflicts with the interrupted module", path.display()).into(),
                );
            }
            if meta.len() == text.len() as u64 {
                continue;
            }
        }
        repairs.push((path, text, entry.is_some()));
    }
    if src_entry.is_none() {
        fs::create_dir(src)?;
    }
    for (path, text, partial) in repairs {
        if partial {
            let seed = staging.join("seed");
            fs::write(&seed, text)?;
            fs::rename(seed, path)?;
        } else {
            File::create_new(path)?.write_all(text.as_bytes())?;
        }
    }
    Ok(())
}

// Install the complete scaffold and its pending manifest together. Moving the
// pending file to build.k then registers the module and clears its retry marker.
fn install_module(directory: &Path, name: &str, staging: &Path, legacy: bool) -> Result<Created> {
    let module_text = format!("module \"{name}\" api=\"src/api.co\"\n");
    let draft = staging.join("build.k");
    let pending = directory.join(PENDING_INCLUDE);
    if let Some(entry) = existing_entry(directory)? {
        if !entry.is_dir() {
            return Err(format!("{} already exists", directory.display()).into());
        }
        match existing_entry(&pending)? {
            Some(marker) if marker.is_file() => {
                let recorded = draft_include(&fs::read(&pending)?);
                if recorded.is_none()
                    || recorded != draft_include(&fs::read(&draft)?)
                    || fs::read_to_string(directory.join("module.k"))? != module_text
                    || !directory.join("src/api.co").is_file()
                {
                    return Err(format!("incomplete module in {}", directory.display()).into());
                }
            }
            None if legacy => finish_legacy_scaffold(directory, &module_text, staging)?,
            _ => return Err(format!("{} already exists", directory.display()).into()),
        }
        fs::rename(draft, pending)?;
        return Ok(Created::default());
    }

    let prepared = staging.join("module");
    fs::create_dir_all(prepared.join("src"))?;
    fs::write(prepared.join("src/api.co"), API)?;
    fs::write(prepared.join("module.k"), module_text)?;
    fs::rename(draft, prepared.join(PENDING_INCLUDE))?;
    fs::rename(&prepared, directory)
        .map_err(|e| format!("creating {}: {e}", directory.display()))?;
    Ok(Created {
        files: vec![
            directory.join("src/api.co"),
            directory.join("module.k"),
            pending,
        ],
        directories: vec![directory.to_owned(), directory.join("src")],
    })
}

fn create_module(name: &str) -> Result<()> {
    let key = manifest::identifier(name)?;
    let manifest = project::enclosing_manifest(Path::new("."))?;
    let root = manifest
        .parent()
        .ok_or("a manifest requires a parent directory")?;
    let _lock = lock_directory(root)?;
    let original =
        fs::read_to_string(&manifest).map_err(|e| format!("{}: {e}", manifest.display()))?;
    project::check_new_module(&manifest, &key)?;
    let updated = with_include(&original, name)?;
    let permissions = fs::metadata(&manifest)?.permissions();
    if permissions.readonly() {
        return Err(format!("{} is read-only", manifest.display()).into());
    }
    let staging = Staging::create(root)?;
    let draft = staging.path.join("build.k");
    fs::write(&draft, &updated)?;
    fs::set_permissions(&draft, permissions)?;
    let directory = root.join(name);
    let legacy = root.join(".build.k.shuttle-init");
    let old_draft = if legacy != manifest && existing_entry(&legacy)?.is_some_and(|m| m.is_file()) {
        fs::read(&legacy).ok()
    } else {
        None
    };
    let old_include = old_draft.as_deref().and_then(draft_include);
    let resuming_legacy = old_include.is_some() && old_include == draft_include(updated.as_bytes());
    let created = install_module(&directory, name, &staging.path, resuming_legacy)?;
    if fs::read_to_string(&manifest)? != original {
        return Err(format!(
            "{} changed during initialization; retry",
            manifest.display()
        )
        .into());
    }
    fs::rename(directory.join(PENDING_INCLUDE), &manifest)
        .map_err(|e| format!("updating {}: {e}", manifest.display()))?;
    created.commit();
    if (old_include.is_none() || resuming_legacy)
        && old_draft.is_some_and(|old| fs::read(&legacy).is_ok_and(|current| current == old))
    {
        let _ = fs::remove_file(legacy);
    }
    println!("Created module '{name}' in {}", directory.display());
    println!("Add uses \"{name}\" to the target or module that imports it.");
    Ok(())
}
