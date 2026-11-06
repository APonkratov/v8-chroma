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
    // Derive offsets only from the trusted embedded DLL. A modified PE
    // header in the on-disk candidate cannot redirect the exclusions.
    let Some(offsets) = timestamp_offsets(embedded) else {
        return false;
    };
    let mut start = 0;
    for offset in offsets {
        if offset < start || embedded[start..offset] != previous[start..offset] {
            return false;
        }
        start = offset + 4;
    }
    embedded[start..] == previous[start..]
}

fn u16_at(data: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        data.get(offset..offset + 2)?.try_into().ok()?,
    ))
}

fn u32_at(data: &[u8], offset: usize) -> Option<usize> {
    Some(u32::from_le_bytes(data.get(offset..offset + 4)?.try_into().ok()?) as usize)
}

/// Locate COFF, export and MSVC POGO timestamps using the embedded PE's tables.
fn timestamp_offsets(data: &[u8]) -> Option<Vec<usize>> {
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
    let directory_count = u32_at(data, optional + 108)?;
    if optional_size < 120 || directory_count == 0 {
        return None;
    }
    let export_rva = u32_at(data, optional + 112)?;
    if export_rva == 0 || u32_at(data, optional + 116)? < 40 {
        return None;
    }
    let section_table = optional + optional_size;
    let section_count = u16_at(data, coff + 2)? as usize;
    let sections_end = section_table + section_count * 40;
    let sections = data.get(section_table..sections_end)?;
    let file_offset = |rva: usize, size: usize| {
        for section in sections.chunks_exact(40) {
            let section_rva = u32_at(section, 12)?;
            let raw_size = u32_at(section, 16)?;
            if let Some(delta) = rva.checked_sub(section_rva)
                && delta <= raw_size
                && size <= raw_size - delta
            {
                let offset = u32_at(section, 20)? + delta;
                return (offset >= sections_end && offset + size <= data.len()).then_some(offset);
            }
        }
        None
    };
    let mut offsets = vec![coff + 4, file_offset(export_rva, 40)? + 4];
    if directory_count > 6 {
        // IMAGE_DIRECTORY_ENTRY_DEBUG is the seventh PE32+ data directory.
        if optional_size < 168 {
            return None;
        }
        let debug_rva = u32_at(data, optional + 160)?;
        let debug_size = u32_at(data, optional + 164)?;
        if debug_rva != 0 || debug_size != 0 {
            const ENTRY_SIZE: usize = 28;
            const POGO: usize = 13;
            if debug_rva == 0 || debug_size == 0 || !debug_size.is_multiple_of(ENTRY_SIZE) {
                return None;
            }
            let debug = file_offset(debug_rva, debug_size)?;
            for entry in (debug..debug + debug_size).step_by(ENTRY_SIZE) {
                if u32_at(data, entry + 12)? == POGO {
                    offsets.push(entry + 4);
                }
            }
        }
    }
    offsets.sort_unstable();
    Some(offsets)
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
            let offsets = timestamp_offsets(embedded).context(name)?;
            let coff = offsets[0];
            for offset in offsets {
                previous[offset] ^= 1;
            }
            let digest = hash(&previous);
            let directory = runtime.path().join(&digest);
            fs::create_dir(&directory)?;
            let file = directory.join(name);
            fs::write(&file, &previous)?;
            assert_eq!(session_path(runtime.path(), name, &digest, embedded)?, file);

            previous[coff] ^= 2;
            fs::write(&file, &previous)?;
            assert!(session_path(runtime.path(), name, &digest, embedded).is_err());

            // A change adjacent to the COFF timestamp still requires a restart,
            // even when the registry and runtime directory carry its correct hash.
            previous[coff + 4] ^= 1;
            let changed_digest = hash(&previous);
            let changed = runtime.path().join(&changed_digest);
            fs::create_dir(&changed)?;
            fs::write(changed.join(name), &previous)?;
            assert!(session_path(runtime.path(), name, &changed_digest, embedded).is_err());
        }
        Ok(())
    }

    #[test]
    fn only_the_pogo_timestamp_is_ignored_in_the_debug_directory() {
        let mut pe = vec![0; 1024];
        pe[..2].copy_from_slice(b"MZ");
        pe[0x3c] = 64;
        pe[64..68].copy_from_slice(b"PE\0\0");
        pe[68..70].copy_from_slice(&0x8664u16.to_le_bytes());
        pe[70] = 1; // One section.
        pe[84] = 240; // Optional header size.
        pe[86..88].copy_from_slice(&0x2000u16.to_le_bytes());
        pe[88..90].copy_from_slice(&0x20bu16.to_le_bytes());
        pe[196] = 7; // NumberOfRvaAndSizes.
        pe[200..204].copy_from_slice(&0x1000u32.to_le_bytes());
        pe[204] = 40;
        pe[248..252].copy_from_slice(&0x1040u32.to_le_bytes());
        pe[252] = 56; // Two IMAGE_DEBUG_DIRECTORY records.
        pe[340..344].copy_from_slice(&0x1000u32.to_le_bytes());
        pe[344..348].copy_from_slice(&512u32.to_le_bytes());
        pe[348..352].copy_from_slice(&512u32.to_le_bytes());
        let pogo = 576;
        let codeview = pogo + 28;
        pe[pogo + 12] = 13;
        pe[codeview + 12] = 2;

        let mut previous = pe.clone();
        previous[72] = 1; // COFF timestamp.
        previous[pogo + 4] = 1;
        assert!(same_except_timestamps(&pe, &previous));
        previous[codeview + 4] = 1;
        assert!(!same_except_timestamps(&pe, &previous));
        previous[codeview + 4] = 0;
        previous[pogo + 12] = 2;
        assert!(!same_except_timestamps(&pe, &previous));
    }
}
