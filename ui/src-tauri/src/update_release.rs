//! GitHub is transport/discovery. Only the embedded Minisign key authorizes bytes.
use anyhow::{Context, Result, bail, ensure};
use minisign_verify::{PublicKey, Signature};
use reqwest::blocking::Client;
use semver::Version;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs::File,
    io::{self, Read, Write},
    path::Path,
    time::Duration,
};

pub const PUBLIC_KEY: &str = include_str!("../update-signing.pub");
pub const SUMS: &str = "SHA256SUMS.txt";
pub const SIGNATURE: &str = "SHA256SUMS.txt.minisig";
pub const GUI: &str = "v8-chroma.exe";
pub const CLI: &str = "v8-chroma-cli.exe";
const API: &str = "https://api.github.com/repos/APonkratov/v8-chroma/releases";
const DOWNLOAD: &str = "https://github.com/APonkratov/v8-chroma/releases/download/";
pub const MAX_EXE: u64 = 128 * 1024 * 1024;

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Release {
    pub tag_name: String,
    #[serde(default)]
    pub body: Option<String>,
    pub draft: bool,
    pub prerelease: bool,
    pub assets: Vec<Asset>,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Asset {
    pub name: String,
    pub browser_download_url: String,
    pub size: u64,
}
#[derive(Clone, Serialize)]
pub struct Available {
    pub version: String,
    pub notes: String,
    pub url: String,
}
impl Release {
    pub fn available(&self) -> Available {
        Available {
            version: self.tag_name.trim_start_matches('v').to_owned(),
            notes: self.body.clone().unwrap_or_default(),
            url: format!(
                "https://github.com/APonkratov/v8-chroma/releases/tag/{}",
                self.tag_name
            ),
        }
    }
    pub fn asset(&self, name: &str) -> Result<&Asset> {
        let matches: Vec<_> = self.assets.iter().filter(|a| a.name == name).collect();
        ensure!(
            matches.len() == 1,
            "В релизе отсутствует или повторяется {name}"
        );
        let asset = matches[0];
        ensure!(
            asset.browser_download_url == format!("{DOWNLOAD}{}/{name}", self.tag_name),
            "Неожиданный адрес файла {name}"
        );
        Ok(asset)
    }
}

pub fn release_version(tag: &str) -> Result<Version> {
    let version = Version::parse(tag.strip_prefix('v').context("Тег без префикса v")?)?;
    ensure!(
        version.build.is_empty(),
        "Метаданные версии не поддерживаются"
    );
    if !version.pre.is_empty() {
        let number = version
            .pre
            .as_str()
            .strip_prefix("rc.")
            .context("Неизвестный канал релиза")?;
        ensure!(
            !number.starts_with('0') && number.parse::<u64>().is_ok_and(|n| n > 0),
            "Некорректный номер RC"
        );
    }
    Ok(version)
}

pub fn select(releases: Vec<Release>, current: &Version, include_rc: bool) -> Option<Release> {
    releases
        .into_iter()
        .filter_map(|release| {
            let version = release_version(&release.tag_name).ok()?;
            if release.draft
                || version <= *current
                || (!include_rc && (release.prerelease || !version.pre.is_empty()))
            {
                return None;
            }
            Some((version, release))
        })
        .max_by(|a, b| a.0.cmp(&b.0))
        .map(|(_, release)| release)
}

pub fn client() -> Result<Client> {
    Client::builder()
        .https_only(true)
        .connect_timeout(Duration::from_secs(10))
        // The blocking client applies this timeout to each read of the EXE stream.
        .timeout(Duration::from_secs(180))
        .user_agent(concat!("v8-chroma/", env!("CARGO_PKG_VERSION")))
        .redirect(reqwest::redirect::Policy::limited(5))
        .build()
        .context("Настройка соединения с GitHub")
}

pub fn fetch(client: &Client, url: &str, limit: u64) -> Result<Vec<u8>> {
    let response = client
        .get(url)
        .timeout(Duration::from_secs(20))
        .header("Accept", "application/vnd.github+json")
        .send()?
        .error_for_status()?;
    ensure!(
        response.content_length().is_none_or(|n| n <= limit),
        "Файл превышает допустимый размер"
    );
    let mut bytes = vec![];
    response.take(limit + 1).read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() as u64 <= limit,
        "Файл превышает допустимый размер"
    );
    Ok(bytes)
}

pub fn discover(client: &Client, include_rc: bool) -> Result<Option<Release>> {
    let mut releases = Vec::new();
    // Bounded pagination: release order is not a SemVer guarantee.
    for page in 1..=3 {
        let bytes = fetch(
            client,
            &format!("{API}?per_page=100&page={page}"),
            4 * 1024 * 1024,
        )?;
        let batch: Vec<Release> = serde_json::from_slice(&bytes)?;
        let last = batch.len() < 100;
        releases.extend(batch);
        if last {
            break;
        }
    }
    Ok(select(
        releases,
        &Version::parse(env!("CARGO_PKG_VERSION"))?,
        include_rc,
    ))
}

pub fn verify_manifest(
    key: &str,
    tag: &str,
    sums: &[u8],
    signature: &str,
) -> Result<BTreeMap<String, String>> {
    release_version(tag)?;
    let key = PublicKey::decode(key.trim()).context("Некорректный открытый ключ обновлений")?;
    let signature = Signature::decode(signature).context("Некорректная подпись обновления")?;
    key.verify(sums, &signature, false)
        .context("Подпись обновления не прошла проверку")?;
    ensure!(
        signature.trusted_comment() == format!("v8-chroma {tag}"),
        "Подпись относится к другой версии"
    );
    let mut hashes = BTreeMap::new();
    for line in std::str::from_utf8(sums)?.lines() {
        let (hash, name) = line
            .split_once("  ")
            .context("Некорректная строка SHA256SUMS.txt")?;
        ensure!(
            hash.len() == 64 && hash.bytes().all(|c| c.is_ascii_hexdigit()),
            "Некорректная SHA-256"
        );
        ensure!(
            !name.is_empty() && !name.contains(['/', '\\', ':']) && name != "." && name != "..",
            "Некорректное имя в SHA256SUMS.txt"
        );
        ensure!(
            hashes
                .insert(name.to_owned(), hash.to_ascii_lowercase())
                .is_none(),
            "Повторяющееся имя в SHA256SUMS.txt"
        );
    }
    ensure!(
        hashes.contains_key(GUI) && hashes.contains_key(CLI),
        "В подписанном списке отсутствуют EXE"
    );
    Ok(hashes)
}

pub fn hash_file(path: &Path) -> Result<String> {
    let mut file = File::open(path)?;
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let n = file.read(&mut buffer)?;
        if n == 0 {
            break;
        }
        hash.update(&buffer[..n]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

pub fn verify_file(path: &Path, expected: &str) -> Result<()> {
    ensure!(
        hash_file(path)? == expected,
        "SHA-256 скачанного файла не совпадает с подписанным списком"
    );
    Ok(())
}

pub fn transient_network_error(error: &anyhow::Error) -> bool {
    !access_denied(error)
        && error.chain().any(|cause| {
            if let Some(error) = cause.downcast_ref::<reqwest::Error>() {
                return error.is_timeout()
                    || error.is_connect()
                    || error
                        .status()
                        .is_some_and(|status| status.is_server_error() || status.as_u16() == 408);
            }
            cause.downcast_ref::<io::Error>().is_some_and(|error| {
                matches!(
                    error.kind(),
                    io::ErrorKind::TimedOut
                        | io::ErrorKind::ConnectionReset
                        | io::ErrorKind::ConnectionAborted
                        | io::ErrorKind::UnexpectedEof
                )
            })
        })
}

fn access_denied(error: &anyhow::Error) -> bool {
    error.chain().any(|cause| {
        cause
            .downcast_ref::<io::Error>()
            .is_some_and(|error| error.kind() == io::ErrorKind::PermissionDenied)
    })
}

#[derive(Debug)]
struct TransferInterrupted;
impl std::fmt::Display for TransferInterrupted {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Загрузка EXE не завершена")
    }
}
impl std::error::Error for TransferInterrupted {}

pub fn download(
    client: &Client,
    asset: &Asset,
    path: &Path,
    hash: &str,
    progress: impl Fn(u64, u64),
) -> Result<()> {
    ensure!(
        asset.size > 0 && asset.size <= MAX_EXE,
        "Недопустимый размер EXE"
    );
    download_with_delays(
        client,
        asset,
        path,
        hash,
        progress,
        &[Duration::from_secs(1), Duration::from_secs(3)],
    )
}

fn download_with_delays(
    client: &Client,
    asset: &Asset,
    path: &Path,
    hash: &str,
    progress: impl Fn(u64, u64),
    delays: &[Duration],
) -> Result<()> {
    let mut delays = delays.iter();
    loop {
        progress(0, asset.size);
        match download_once(client, asset, path, hash, &progress) {
            Err(error) if error.is::<TransferInterrupted>() => match delays.next() {
                Some(delay) => std::thread::sleep(*delay),
                None => return Err(error),
            },
            result => return result,
        }
    }
}

fn download_once(
    client: &Client,
    asset: &Asset,
    path: &Path,
    hash: &str,
    progress: impl Fn(u64, u64),
) -> Result<()> {
    let mut response = client
        .get(&asset.browser_download_url)
        .send()
        .and_then(reqwest::blocking::Response::error_for_status)
        .map_err(|error| {
            let error = anyhow::Error::from(error);
            if transient_network_error(&error) {
                error.context(TransferInterrupted)
            } else {
                error
            }
        })?;
    let mut file = File::create(path)?;
    let mut size = 0;
    let mut buffer = [0u8; 65536];
    loop {
        let n = response.read(&mut buffer).map_err(|error| {
            let error = anyhow::Error::from(error);
            if access_denied(&error) {
                error
            } else {
                error.context(TransferInterrupted)
            }
        })?;
        if n == 0 {
            break;
        }
        size += n as u64;
        if size > asset.size {
            bail!("EXE превышает размер, указанный в релизе");
        }
        file.write_all(&buffer[..n])?;
        progress(size, asset.size);
    }
    ensure!(size == asset.size, TransferInterrupted);
    file.sync_all()?;
    drop(file);
    verify_file(path, hash)
}

#[cfg(test)]
mod tests {
    use super::*;
    const TEST_KEY: &str = include_str!("../tests/fixtures/updates/test.pub");
    const VALID: &[u8] = include_bytes!("../tests/fixtures/updates/valid.txt");
    const SIG: &str = include_str!("../tests/fixtures/updates/valid.txt.minisig");
    #[test]
    fn authentic_manifest_and_file_are_accepted() {
        let hashes = verify_manifest(TEST_KEY, "v9.0.0", VALID, SIG).unwrap();
        let file = tempfile::NamedTempFile::new().unwrap();
        std::fs::write(file.path(), b"new gui").unwrap();
        verify_file(file.path(), &hashes[GUI]).unwrap();
        std::fs::write(file.path(), b"tampered").unwrap();
        assert!(verify_file(file.path(), &hashes[GUI]).is_err());
    }
    #[test]
    fn interrupted_exe_download_restarts_but_bad_hash_and_access_fail_immediately() -> Result<()> {
        use std::{cell::Cell, net::TcpListener, time::Instant};

        let valid = b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nnew gui";
        let cut = b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nnew";
        let denied = b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        let hash = verify_manifest(TEST_KEY, "v9.0.0", VALID, SIG)?;
        let client = Client::builder()
            .no_proxy()
            .timeout(Duration::from_secs(2))
            .build()?;
        for (responses, expected_hash, error_message) in [
            (
                vec![cut.as_slice(), valid.as_slice()],
                hash[GUI].as_str(),
                None,
            ),
            (
                vec![cut.as_slice(); 3],
                hash[GUI].as_str(),
                Some("Загрузка EXE не завершена"),
            ),
            (vec![valid.as_slice()], &hash[CLI], Some("SHA-256")),
            (vec![denied.as_slice()], &hash[GUI], Some("403")),
        ] {
            let listener = TcpListener::bind(("127.0.0.1", 0))?;
            listener.set_nonblocking(true)?;
            let url = format!("http://{}/file.exe", listener.local_addr()?);
            let requests = responses.len();
            let server = std::thread::spawn(move || -> io::Result<()> {
                let deadline = Instant::now() + Duration::from_secs(5);
                for response in responses {
                    let mut stream = loop {
                        match listener.accept() {
                            Ok((stream, _)) => break stream,
                            Err(error)
                                if error.kind() == io::ErrorKind::WouldBlock
                                    && Instant::now() < deadline =>
                            {
                                std::thread::sleep(Duration::from_millis(5));
                            }
                            Err(error) => return Err(error),
                        }
                    };
                    // Winsock inherits the listener's nonblocking mode.
                    stream.set_nonblocking(false)?;
                    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
                    let mut request = [0u8; 1024];
                    assert!(stream.read(&mut request)? > 0);
                    stream.write_all(response)?;
                }
                Ok(())
            });
            let asset = Asset {
                name: GUI.into(),
                browser_download_url: url,
                size: 7,
            };
            let temp = tempfile::tempdir()?;
            let path = temp.path().join("new.exe");
            let attempts = Cell::new(0);
            let result = download_with_delays(
                &client,
                &asset,
                &path,
                expected_hash,
                |size, _| {
                    if size == 0 {
                        attempts.set(attempts.get() + 1);
                    }
                },
                &[Duration::ZERO; 2],
            );
            server.join().unwrap()?;
            assert_eq!(attempts.get(), requests);
            if let Some(message) = error_message {
                assert!(format!("{:#}", result.unwrap_err()).contains(message));
            } else {
                result?;
                assert_eq!(std::fs::read(path)?, b"new gui");
            }
        }
        Ok(())
    }
    #[test]
    fn tampering_wrong_version_and_wrong_key_are_rejected() {
        let mut changed = VALID.to_vec();
        changed[0] = if changed[0] == b'1' { b'2' } else { b'1' };
        assert!(verify_manifest(TEST_KEY, "v9.0.0", &changed, SIG).is_err());
        assert!(verify_manifest(TEST_KEY, "v9.0.1", VALID, SIG).is_err());
        assert!(verify_manifest(PUBLIC_KEY, "v9.0.0", VALID, SIG).is_err());
        let changed_sig = SIG.replace("v9.0.0", "v9.0.1");
        assert!(verify_manifest(TEST_KEY, "v9.0.1", VALID, &changed_sig).is_err());
    }
    #[test]
    fn even_valid_signatures_do_not_allow_ambiguous_or_incomplete_manifests() {
        for (sums, sig) in [
            (
                &include_bytes!("../tests/fixtures/updates/duplicate.txt")[..],
                include_str!("../tests/fixtures/updates/duplicate.txt.minisig"),
            ),
            (
                &include_bytes!("../tests/fixtures/updates/traversal.txt")[..],
                include_str!("../tests/fixtures/updates/traversal.txt.minisig"),
            ),
            (
                &include_bytes!("../tests/fixtures/updates/missing.txt")[..],
                include_str!("../tests/fixtures/updates/missing.txt.minisig"),
            ),
        ] {
            assert!(verify_manifest(TEST_KEY, "v9.0.0", sums, sig).is_err());
        }
    }
    fn candidate(tag: &str, draft: bool, prerelease: bool) -> Release {
        Release {
            tag_name: tag.into(),
            draft,
            prerelease,
            body: None,
            assets: vec![],
        }
    }
    #[test]
    fn channels_semver_order_and_no_downgrade() {
        let releases = vec![
            candidate("v1.9.0", false, false),
            candidate("v1.10.0", false, false),
            candidate("v2.0.0-rc.1", false, true),
            candidate("v3.0.0", true, false),
            candidate("v9.0.0-beta.1", false, true),
        ];
        let current = Version::parse("1.0.0").unwrap();
        assert_eq!(
            select(releases.clone(), &current, false).unwrap().tag_name,
            "v1.10.0"
        );
        assert_eq!(
            select(releases.clone(), &current, true).unwrap().tag_name,
            "v2.0.0-rc.1"
        );
        assert!(select(releases, &Version::parse("2.0.0").unwrap(), true).is_none());
        assert!(
            select(
                vec![candidate("v2.0.0", false, false)],
                &Version::parse("2.0.0-rc.1").unwrap(),
                false
            )
            .is_some()
        );
        for tag in [
            "1.0.0",
            "v01.0.0",
            "v1.0.0+build",
            "v1.0.0-rc.01",
            "v1.0.0-rc.0",
            "v1.0.0-rc.1.extra",
        ] {
            assert!(release_version(tag).is_err(), "{tag}");
        }
    }
    #[test]
    fn assets_are_bound_to_the_selected_release() {
        let mut r = candidate("v9.0.0", false, false);
        r.assets.push(Asset {
            name: GUI.into(),
            size: 1,
            browser_download_url: format!("{DOWNLOAD}v9.0.0/{GUI}"),
        });
        assert!(r.asset(GUI).is_ok());
        r.assets[0].browser_download_url = "https://example.org/evil.exe".into();
        assert!(r.asset(GUI).is_err());
        r.assets.push(r.assets[0].clone());
        assert!(r.asset(GUI).is_err());
    }
}
