// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use crate::Result;
use object::{Object, ObjectKind, ObjectSection};
use std::collections::BTreeMap;
use std::str;

pub struct Interfaces<'a> {
    target: &'a str,
    producer: &'a str,
    modules: BTreeMap<&'a str, &'a str>,
}

impl<'a> Interfaces<'a> {
    pub fn read(bytes: &'a [u8]) -> Result<Self> {
        let object = object::File::parse(bytes)?;
        if object.kind() != ObjectKind::Relocatable {
            return Err("expected a relocatable Cloth object".into());
        }
        let section = object
            .section_by_name(".clapi")
            .ok_or("object has no Cloth interface record")?;
        let mut lines = str::from_utf8(section.data()?)?
            .trim_end_matches('\0')
            .lines();
        if lines.next() != Some("cloth-interface-v1") {
            return Err("unsupported Cloth interface record".into());
        }
        let target = lines
            .next()
            .filter(|s| !s.is_empty())
            .ok_or("missing interface target")?;
        let producer = lines.next().ok_or("missing interface producer")?;
        let mut modules = BTreeMap::new();
        for line in lines {
            let (module, exports) = line.split_once('\t').ok_or("invalid module interface")?;
            if module.is_empty() || modules.insert(module, exports).is_some() {
                return Err("invalid module name in interface record".into());
            }
        }
        Ok(Self {
            target,
            producer,
            modules,
        })
    }

    pub fn check_library(&self, module: &str, bytes: &[u8]) -> Result<()> {
        let library = Interfaces::read(bytes)?;
        if library.producer != module || !library.modules.contains_key(module) {
            return Err(format!("expected an object for module '{module}'").into());
        }
        if library.target != self.target {
            return Err(format!(
                "object targets '{}' but the executable targets '{}'",
                library.target, self.target
            )
            .into());
        }
        for (name, exports) in library.modules {
            if self.modules.get(name) != Some(&exports) {
                return Err(format!(
                    "native interface for '{name}' differs from the checked source"
                )
                .into());
            }
        }
        Ok(())
    }
}
