use crate::manager::hash;
use anyhow::{Result, ensure};
use std::{
    fs::File,
    io::Read,
    path::{Path, PathBuf},
};

/// Resolve the DLL recorded for a running process. The digest still identifies
/// the exact file on disk, even when its linker timestamps differ from ours.
pub(crate) fn session_path(
    runtime: &Path,
    name: &str,
    digest: &str,
    embedded: &[u8],
) -> Result<PathBuf> {
    ensure!(
        digest.len() == 64
            && digest
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
        "Некорректная контрольная сумма движка"
    );
    let path = runtime.join(digest).join(name);
    if digest != hash(embedded) {
        let mut previous = Vec::new();
        File::open(&path)?
            .take(embedded.len() as u64 + 1)
            .read_to_end(&mut previous)?;
        ensure!(
            same_except_timestamps(embedded, &previous) && hash(&previous) == digest,
            "Содержимое DLL отличается от встроенного движка или контрольной суммы сессии"
        );
    }
    Ok(path)
}

fn same_except_timestamps(embedded: &[u8], previous: &[u8]) -> bool {
    if embedded.len() != previous.len() {
        return false;
    }
    // Derive both offsets only from the trusted embedded DLL. A modified PE
    // header in the on-disk candidate cannot redirect the exclusions.
    let Some([coff, exports]) = timestamp_offsets(embedded) else {
        return false;
    };
    embedded[..coff] == previous[..coff]
        && embedded[coff + 4..exports] == previous[coff + 4..exports]
        && embedded[exports + 4..] == previous[exports + 4..]
}

fn u16_at(data: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        data.get(offset..offset + 2)?.try_into().ok()?,
    ))
}

fn u32_at(data: &[u8], offset: usize) -> Option<usize> {
    Some(u32::from_le_bytes(data.get(offset..offset + 4)?.try_into().ok()?) as usize)
}

fn timestamp_offsets(data: &[u8]) -> Option<[usize; 2]> {
    if data.get(..2)? != b"MZ" {
        return None;
    }
    let pe = u32_at(data, 0x3c)?;
    let coff = pe + 4;
    let optional = coff + 20;
    if pe < 64
        || data.get(pe..coff)? != b"PE\0\0"
        || u16_at(data, coff)? != 0x8664
        || u16_at(data, coff + 18)? & 0x2000 == 0
        || u16_at(data, optional)? != 0x20b
    {
        return None;
    }
    let optional_size = u16_at(data, coff + 16)? as usize;
    if optional_size < 120 || u32_at(data, optional + 108)? == 0 {
        return None;
    }
    let export_rva = u32_at(data, optional + 112)?;
    if export_rva == 0 || u32_at(data, optional + 116)? < 40 {
        return None;
    }
    let section_table = optional + optional_size;
    let section_count = u16_at(data, coff + 2)? as usize;
    let sections_end = section_table + section_count * 40;
    for section in data.get(section_table..sections_end)?.chunks_exact(40) {
        let section_rva = u32_at(section, 12)?;
        let raw_size = u32_at(section, 16)?;
        if let Some(delta) = export_rva.checked_sub(section_rva)
            && delta + 40 <= raw_size
        {
            let export_offset = u32_at(section, 20)? + delta;
            if export_offset >= sections_end && export_offset + 40 <= data.len() {
                return Some([coff + 4, export_offset + 4]);
            }
            return None;
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Context;
    use std::fs;

    #[test]
    fn only_linker_timestamps_may_differ() -> Result<()> {
        for (name, embedded) in [
            ("ThemeProbe.dll", crate::theme::DLL),
            (
                "FontProbe.dll",
                include_bytes!("../assets/FontProbe.dll").as_slice(),
            ),
            (
                "UiFontEngine.dll",
                include_bytes!("../assets/UiFontEngine.dll").as_slice(),
            ),
        ] {
            let runtime = tempfile::tempdir()?;
            let mut previous = embedded.to_vec();
            let [coff, exports] = timestamp_offsets(embedded).context(name)?;
            previous[coff] ^= 1;
            previous[exports] ^= 1;
            let digest = hash(&previous);
            let directory = runtime.path().join(&digest);
            fs::create_dir(&directory)?;
            let file = directory.join(name);
            fs::write(&file, &previous)?;
            assert_eq!(session_path(runtime.path(), name, &digest, embedded)?, file);

            previous[coff] ^= 2;
            fs::write(&file, &previous)?;
            assert!(session_path(runtime.path(), name, &digest, embedded).is_err());

            // A change adjacent to the export timestamp still requires a restart,
            // even when the registry and runtime directory carry its correct hash.
            previous[exports + 4] ^= 1;
            let changed_digest = hash(&previous);
            let changed = runtime.path().join(&changed_digest);
            fs::create_dir(&changed)?;
            fs::write(changed.join(name), &previous)?;
            assert!(session_path(runtime.path(), name, &changed_digest, embedded).is_err());
        }
        Ok(())
    }
}
