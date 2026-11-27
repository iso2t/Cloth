// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::Result;
use std::fs;
use std::io::{self, ErrorKind};
use std::path::{Path, PathBuf};
use std::process;

pub(crate) struct Staging {
    pub(crate) path: PathBuf,
    keep: bool,
}

// Keep the file between operations so waiters and new arrivals lock the same
// file. Closing the handle, including on process exit, releases the lock.
pub(crate) fn lock_directory(directory: &Path) -> Result<fs::File> {
    let path = directory.join(".shuttle.lock");
    let file = fs::File::options()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(&path)
        .map_err(|e| format!("opening {}: {e}", path.display()))?;
    fs2::FileExt::lock_exclusive(&file).map_err(|e| format!("locking {}: {e}", path.display()))?;
    Ok(file)
}

struct Replacement<'a> {
    staged: &'a Path,
    artifact: &'a Path,
    previous: Option<PathBuf>,
    changed: bool,
}

fn artifact_type(path: &Path) -> io::Result<Option<fs::FileType>> {
    match fs::symlink_metadata(path) {
        Ok(entry) if entry.is_file() || entry.is_symlink() => Ok(Some(entry.file_type())),
        Ok(_) => Err(io::Error::new(ErrorKind::InvalidInput, "expected a file")),
        Err(e) if e.kind() == ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

impl<'a> Replacement<'a> {
    fn prepare(staged: &'a Path, artifact: &'a Path) -> Result<Self> {
        artifact_type(artifact).map_err(|e| format!("publishing {}: {e}", artifact.display()))?;
        Ok(Self {
            staged,
            artifact,
            previous: None,
            changed: false,
        })
    }

    fn install(&mut self, backups: &Path) -> io::Result<()> {
        if let Some(kind) = artifact_type(self.artifact)? {
            let backup = self
                .previous
                .insert(backups.join(self.artifact.file_name().unwrap()))
                .as_path();
            // A hard link keeps the old file visible until replacement. Moving it also
            // preserves symlinks verbatim and works on filesystems without hard links.
            if !kind.is_file() || fs::hard_link(self.artifact, backup).is_err() {
                fs::rename(self.artifact, backup)?;
                self.changed = true;
                // If another writer changed the entry during the move, roll it back before installing anything over it.
                if fs::symlink_metadata(backup)?.file_type() != kind {
                    return Err(io::Error::other("output changed type during publication"));
                }
            }
        }
        fs::rename(self.staged, self.artifact)?;
        self.changed = true;
        Ok(())
    }

    fn restore(&self) -> io::Result<()> {
        let current = artifact_type(self.artifact)?;
        match &self.previous {
            Some(backup) => fs::rename(backup, self.artifact),
            None if current.is_some() => fs::remove_file(self.artifact),
            None => Ok(()),
        }
    }
}

impl Staging {
    pub(crate) fn create(directory: &Path) -> Result<Self> {
        fs::create_dir_all(directory)?;
        for attempt in 0.. {
            let path = directory.join(format!(".shuttle-{}-{attempt}", process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(Self { path, keep: false }),
                Err(error) if error.kind() == ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error.into()),
            }
        }
        unreachable!()
    }

    pub(crate) fn publish(
        mut self,
        output_root: &Path,
        outputs: &[(PathBuf, PathBuf)],
    ) -> Result<()> {
        let _lock = lock_directory(output_root)?;

        if let [(staged, artifact)] = outputs {
            return artifact_type(artifact)
                .and_then(|_| fs::rename(staged, artifact))
                .map_err(|e| format!("publishing {}: {e}", artifact.display()).into());
        }

        let backups = self.path.join("previous");
        fs::create_dir(&backups)?;
        let mut replacements = outputs
            .iter()
            .map(|(staged, artifact)| Replacement::prepare(staged, artifact))
            .collect::<Result<Vec<_>>>()?;
        replacements
            .iter_mut()
            .try_for_each(|entry| {
                entry
                    .install(&backups)
                    .map_err(|e| format!("publishing {}: {e}", entry.artifact.display()))
            })
            .map_err(|message| self.rollback(&replacements, &backups, message).into())
    }

    fn rollback(
        &mut self,
        replacements: &[Replacement<'_>],
        backups: &Path,
        mut message: String,
    ) -> String {
        for entry in replacements.iter().rev().filter(|entry| entry.changed) {
            if let Err(e) = entry.restore() {
                self.keep = true;
                message.push_str(&format!("\nrestoring {}: {e}", entry.artifact.display()));
            }
        }
        if self.keep {
            message.push_str(&format!(
                "\nprevious artifacts retained in {}",
                backups.display()
            ));
        }
        message
    }
}

impl Drop for Staging {
    fn drop(&mut self) {
        if !self.keep {
            let _ = fs::remove_dir_all(&self.path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_late_rename_failure_restores_replacements_and_removes_new_files() -> Result<()> {
        let work = Staging::create(&std::env::temp_dir())?;
        let directory = work.path.join("target");
        let staging = Staging::create(&directory)?;
        let staging_path = staging.path.clone();
        let outputs =
            ["old", "new", "last"].map(|name| (staging.path.join(name), directory.join(name)));
        fs::write(&outputs[0].1, b"previous")?;
        fs::write(&outputs[2].1, b"last good")?;
        fs::write(&outputs[0].0, b"replacement")?;
        fs::write(&outputs[1].0, b"first build")?;
        // The last staged file is missing. Its rename fails after the first two succeed.
        let error = staging.publish(&work.path, &outputs).unwrap_err();
        assert!(
            error
                .to_string()
                .contains(&outputs[2].1.display().to_string())
        );
        assert_eq!(fs::read(&outputs[0].1)?, b"previous");
        assert!(!outputs[1].1.try_exists()?);
        assert_eq!(fs::read(&outputs[2].1)?, b"last good");
        assert!(!staging_path.try_exists()?);
        Ok(())
    }

    #[test]
    fn a_blocked_restore_keeps_the_backup_and_undoes_the_other_replacements() -> Result<()> {
        let work = Staging::create(&std::env::temp_dir())?;
        let mut staging = Staging::create(&work.path)?;
        let backups = staging.path.join("previous");
        fs::create_dir(&backups)?;
        let old = work.path.join("app");
        let new = work.path.join("new");
        let staged_new = staging.path.join("new");
        let staged_old = staging.path.join("app");
        fs::write(&old, b"last good")?;
        fs::write(&staged_new, b"first build")?;
        fs::write(&staged_old, b"replacement")?;
        let mut replacements = [
            Replacement::prepare(&staged_new, &new)?,
            Replacement::prepare(&staged_old, &old)?,
        ];
        for entry in &mut replacements {
            entry.install(&backups)?;
        }
        // Another writer has put a directory where the executable must be restored.
        fs::remove_file(&old)?;
        fs::create_dir(&old)?;
        fs::write(old.join("keep"), b"user file")?;
        let message = staging.rollback(&replacements, &backups, "publication failed".into());
        drop(staging);
        assert!(message.contains(&old.display().to_string()));
        assert!(message.contains(&backups.display().to_string()));
        assert_eq!(fs::read(backups.join("app"))?, b"last good");
        assert_eq!(fs::read(old.join("keep"))?, b"user file");
        assert!(!new.try_exists()?);
        Ok(())
    }

    #[test]
    fn a_destination_that_becomes_a_directory_stays_in_place() -> Result<()> {
        let work = Staging::create(&std::env::temp_dir())?;
        let staging = Staging::create(&work.path)?;
        let backups = staging.path.join("previous");
        fs::create_dir(&backups)?;
        let artifact = work.path.join("app");
        let staged = staging.path.join("app");
        fs::write(&artifact, b"previous")?;
        fs::write(&staged, b"replacement")?;
        let mut entry = Replacement::prepare(&staged, &artifact)?;
        fs::remove_file(&artifact)?;
        fs::create_dir(&artifact)?;
        fs::write(artifact.join("keep"), b"user file")?;
        assert!(entry.install(&backups).is_err());
        assert!(!entry.changed);
        drop(staging);
        assert_eq!(fs::read(artifact.join("keep"))?, b"user file");
        Ok(())
    }

    #[test]
    fn a_failed_publisher_cannot_undo_a_concurrent_single_target_build() -> Result<()> {
        use std::sync::{Barrier, mpsc};
        use std::time::Duration;

        let work = Staging::create(&std::env::temp_dir())?;
        let directory = work.path.join("target");
        let shared = directory.join("app");
        let single = Staging::create(&directory)?;
        let multiple = Staging::create(&directory)?;
        let good = [(single.path.join("app"), shared.clone())];
        let bad = [
            (multiple.path.join("app"), shared.clone()),
            (multiple.path.join("missing"), directory.join("other")),
        ];
        fs::write(&good[0].0, b"successful build")?;
        fs::write(&bad[0].0, b"incomplete build")?;

        let lock = fs::File::create(work.path.join(".shuttle.lock"))?;
        fs2::FileExt::lock_exclusive(&lock)?;
        let ready = Barrier::new(3);
        let (done, finished) = mpsc::channel();
        std::thread::scope(|scope| {
            scope.spawn(|| {
                ready.wait();
                let result = single.publish(&work.path, &good);
                done.send(()).unwrap();
                result.unwrap();
            });
            scope.spawn(|| {
                ready.wait();
                let result = multiple.publish(&work.path, &bad);
                done.send(()).unwrap();
                result.unwrap_err();
            });
            ready.wait();
            let waiting = finished.recv_timeout(Duration::from_millis(100));
            drop(lock);
            assert_eq!(waiting, Err(mpsc::RecvTimeoutError::Timeout));
        });
        assert_eq!(fs::read(shared)?, b"successful build");
        assert!(!bad[1].1.try_exists()?);
        Ok(())
    }

    #[cfg(unix)]
    #[test]
    fn rollback_restores_a_relative_symlink_including_a_dangling_one() -> Result<()> {
        use std::os::unix::fs::symlink;

        let work = Staging::create(&std::env::temp_dir())?;
        let directory = work.path.join("target");
        let staging = Staging::create(&directory)?;
        let outputs = ["link", "dangling", "last"]
            .map(|name| (staging.path.join(name), directory.join(name)));
        fs::write(directory.join("original"), b"previous")?;
        symlink("original", &outputs[0].1)?;
        symlink("absent", &outputs[1].1)?;
        fs::write(&outputs[0].0, b"replacement")?;
        fs::write(&outputs[1].0, b"replacement")?;
        assert!(staging.publish(&work.path, &outputs).is_err());
        assert_eq!(fs::read_link(&outputs[0].1)?, Path::new("original"));
        assert_eq!(fs::read_link(&outputs[1].1)?, Path::new("absent"));
        assert_eq!(fs::read(&outputs[0].1)?, b"previous");
        assert!(!directory.join("absent").try_exists()?);
        Ok(())
    }
}
