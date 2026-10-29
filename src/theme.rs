use anyhow::{Result, bail, ensure};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet},
    path::Path,
};

const PROFILE: &str = include_str!("../assets/profile.properties");
#[cfg(all(
    target_os = "windows",
    target_arch = "x86_64",
    feature = "windows-engine"
))]
pub const DLL: &[u8] = include_bytes!("../assets/ThemeProbe.dll");
pub const IDS: &[&str] = &[
    "ayu-dark",
    "darcula",
    "dracula",
    "github-dark-default",
    "github-light-default",
    "monokai",
    "night-owl",
    "noctis-minimus",
    "one-dark-pro",
    "shades-of-purple",
    "visual-studio-dark-cpp",
    "visual-studio-light",
    "winter-is-coming-dark-blue",
];
const BINDINGS: &[&str] = &[
    "glyph.background",
    "glyph.foreground",
    "glyph.selection.background",
    "glyph.selection.foreground",
    "raster.background",
    "raster.foreground",
    "window.caption",
    "window.text",
    "window.border",
];
const ROLES: &[&str] = &[
    "editor.background",
    "text",
    "panel",
    "border",
    "selection.background",
    "selection.text",
    "line",
    "comment",
    "keyword",
    "string",
    "number",
    "directive",
    "accent",
    "error",
    "warning",
    "disabled",
    "hover",
];

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ColorMap {
    pub kind: u32,
    pub source: u32,
    pub target: u32,
}

#[repr(C)]
#[derive(Clone)]
pub struct Config {
    pub size: u32,
    pub version: u32,
    pub mode: u32,
    pub map_count: u32,
    pub theme_id: [u8; 64],
    pub colors: [u32; 10],
    pub target_dir: [u16; 260],
    pub log_path: [u16; 520],
    pub maps: [ColorMap; 256],
    pub icon_mode: u32,
    pub icon_pack: u32,
}
const _: () = assert!(std::mem::size_of::<Config>() == 4760);

impl Config {
    pub fn bytes(&self) -> &[u8] {
        // repr(C), fixed-width integer fields, and a verified padding-free v3 layout.
        unsafe { std::slice::from_raw_parts(self as *const Self as *const u8, size_of::<Self>()) }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Document {
    format_version: u32,
    id: String,
    name: String,
    colors: BTreeMap<String, String>,
    render: BTreeMap<String, serde_json::Value>,
}

#[derive(Clone, Serialize)]
pub struct Preview {
    pub id: String,
    pub name: String,
    pub colors: BTreeMap<String, String>,
    pub editor: BTreeMap<String, String>,
    pub dark: bool,
}
pub fn catalog() -> Result<Vec<Preview>> {
    IDS.iter()
        .map(|id| {
            let cfg = config(id, Path::new("x"), Path::new("y"))?;
            let doc: Document = serde_json::from_str(source(id)?)?;
            Ok(Preview {
                id: doc.id,
                name: doc.name,
                dark: doc.render["window.dark"].as_bool().unwrap_or(false),
                editor: editor_preview(&cfg),
                colors: doc.colors,
            })
        })
        .collect()
}

/// Effective colors of the default 8.3.27 BSL editor, resolved through the exact
/// native mapping table. The renderer receives RGB values, not semantic tokens.
fn editor_preview(cfg: &Config) -> BTreeMap<String, String> {
    [
        ("keyword", 0xFF0000),
        ("punctuation", 0xFF0000),
        ("identifier", 0x0000FF),
        ("builtin", 0x0000FF),
        ("string", 0x000000),
        ("number", 0x000000),
        ("comment", 0x008000),
        ("directive", 0x963200),
    ]
    .into_iter()
    .map(|(token, source)| {
        let color = cfg.maps[..cfg.map_count as usize]
            .iter()
            .find(|map| map.kind == 6 && map.source == source)
            .map_or(source, |map| map.target);
        (token.to_owned(), format!("#{color:06X}"))
    })
    .collect()
}

fn source(id: &str) -> Result<&'static str> {
    macro_rules! presets { ($($id:literal),*) => { match id {
        $($id => Ok(include_str!(concat!("../assets/themes/", $id, ".json"))),)*
        _ => bail!("Неизвестная тема: {id}"),
    } }; }
    presets!(
        "ayu-dark",
        "darcula",
        "dracula",
        "github-dark-default",
        "github-light-default",
        "monokai",
        "night-owl",
        "noctis-minimus",
        "one-dark-pro",
        "shades-of-purple",
        "visual-studio-dark-cpp",
        "visual-studio-light",
        "winter-is-coming-dark-blue"
    )
}

fn wide<const N: usize>(path: &Path) -> Result<[u16; N]> {
    #[cfg(windows)]
    use std::os::windows::ffi::OsStrExt;
    #[cfg(windows)]
    let text: Vec<u16> = path.as_os_str().encode_wide().collect();
    #[cfg(not(windows))]
    let text: Vec<u16> = path
        .to_str()
        .ok_or_else(|| anyhow::anyhow!("Путь DLL-протокола должен быть представлен в Unicode"))?
        .encode_utf16()
        .collect();
    ensure!(
        text.len() < N && !text.contains(&0),
        "Превышен предел длины пути DLL-протокола"
    );
    let mut result = [0; N];
    result[..text.len()].copy_from_slice(&text);
    Ok(result)
}

pub fn config(id: &str, target: &Path, log: &Path) -> Result<Config> {
    let mut cfg = Config {
        size: 4760,
        version: 3,
        mode: 0,
        map_count: 0,
        theme_id: [0; 64],
        colors: [0; 10],
        target_dir: wide(target)?,
        log_path: wide(log)?,
        maps: [ColorMap::default(); 256],
        icon_mode: 0,
        icon_pack: 0,
    };
    ensure!(id.len() < 64, "Слишком длинный идентификатор темы");
    cfg.theme_id[..id.len()].copy_from_slice(id.as_bytes());
    if id == "diagnostic" || id == "standard" {
        return Ok(cfg);
    }
    let doc: Document = serde_json::from_str(source(id)?)?;
    ensure!(
        doc.format_version == 1 && doc.id == id && !doc.name.trim().is_empty(),
        "Некорректная встроенная тема"
    );
    ensure!(
        doc.colors.len() == ROLES.len() && ROLES.iter().all(|k| doc.colors.contains_key(*k)),
        "Некорректные роли темы"
    );
    ensure!(
        doc.render.len() == 10 && doc.render.contains_key("window.dark"),
        "Некорректные render bindings"
    );
    let mut colors: BTreeMap<&str, u32> = doc
        .colors
        .iter()
        .map(|(k, v)| {
            ensure!(
                v.len() == 7 && v.starts_with('#') && v[1..].bytes().all(|c| c.is_ascii_hexdigit()),
                "Некорректный RGB"
            );
            Ok((k.as_str(), u32::from_str_radix(&v[1..], 16)?))
        })
        .collect::<Result<_>>()?;
    for (i, key) in BINDINGS.iter().enumerate() {
        let role = doc
            .render
            .get(*key)
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow::anyhow!("Некорректный binding {key}"))?;
        cfg.colors[i] = *colors
            .get(role)
            .ok_or_else(|| anyhow::anyhow!("Неизвестная роль {role}"))?;
    }
    cfg.colors[9] = u32::from(
        doc.render["window.dark"]
            .as_bool()
            .ok_or_else(|| anyhow::anyhow!("window.dark не bool"))?,
    );
    for (role, color) in comparison_backgrounds(colors["editor.background"], colors["text"]) {
        colors.insert(role, color);
    }
    let link = readable_link(
        colors["accent"],
        colors["text"],
        [colors["editor.background"], colors["panel"]],
    );
    colors.insert("link", link);
    ensure!(
        cfg.colors[0] != cfg.colors[2] || cfg.colors[1] == cfg.colors[3],
        "Неоднозначные цвета глифов"
    );
    let kinds = [
        "text",
        "background",
        "brush",
        "pen",
        "cairo-rgb",
        "cairo-rgba",
        "color-stop",
        "mesh",
    ];
    let mut seen = BTreeSet::new();
    for raw in PROFILE.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (key, role) = line
            .split_once('=')
            .ok_or_else(|| anyhow::anyhow!("Некорректный профиль"))?;
        let (kind, rgb) = key
            .trim()
            .split_once('.')
            .ok_or_else(|| anyhow::anyhow!("Некорректный mapping"))?;
        let kind = kinds
            .iter()
            .position(|k| *k == kind)
            .ok_or_else(|| anyhow::anyhow!("Неизвестный API"))? as u32
            + 1;
        ensure!(
            rgb.len() == 6 && rgb.bytes().all(|c| c.is_ascii_hexdigit()),
            "Некорректный source RGB"
        );
        let source = u32::from_str_radix(rgb, 16)?;
        ensure!(
            seen.insert((kind, source)) && cfg.map_count < 256,
            "Повторный/слишком большой профиль"
        );
        cfg.maps[cfg.map_count as usize] = ColorMap {
            kind,
            source,
            target: *colors
                .get(role.trim())
                .ok_or_else(|| anyhow::anyhow!("Неизвестная роль профиля"))?,
        };
        cfg.map_count += 1;
    }
    ensure!(cfg.map_count > 0, "Пустой профиль");
    cfg.mode = 1;
    Ok(cfg)
}

/// Complete appearance request; diagnostic mode never transforms images.
pub fn config_with_icons(
    id: &str,
    target: &Path,
    log: &Path,
    custom_icons: bool,
) -> Result<Config> {
    let mut cfg = config(id, target, log)?;
    if custom_icons && id != "diagnostic" {
        cfg.icon_mode = if id == "standard" || cfg.colors[9] == 0 {
            1
        } else {
            2
        };
        cfg.icon_pack = 1;
    }
    Ok(cfg)
}

fn luminance(rgb: u32) -> f64 {
    let linear = |shift: u32| {
        let value = f64::from((rgb >> shift) & 255) / 255.0;
        if value <= 0.04045 {
            value / 12.92
        } else {
            ((value + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * linear(16) + 0.7152 * linear(8) + 0.0722 * linear(0)
}

fn contrast(left: u32, right: u32) -> f64 {
    let (left, right) = (luminance(left), luminance(right));
    (left.max(right) + 0.05) / (left.min(right) + 0.05)
}

// Retain the theme's accent where readable; otherwise move toward its text ink.
fn readable_link(accent: u32, text: u32, backgrounds: [u32; 2]) -> u32 {
    (0..=100)
        .map(|amount| {
            [16, 8, 0].into_iter().fold(0, |rgb, shift| {
                rgb | (((((accent >> shift) & 255) * (100 - amount)
                    + ((text >> shift) & 255) * amount
                    + 50)
                    / 100)
                    << shift)
            })
        })
        .find(|color| backgrounds.iter().all(|bg| contrast(*color, *bg) >= 4.5))
        .unwrap_or(text)
}

// Derived profile roles keep the existing public EDT JSON schema unchanged.
fn comparison_backgrounds(background: u32, text: u32) -> [(&'static str, u32); 3] {
    [
        ("compare.changed", 0x00B8C4),
        ("compare.local", 0xE99A49),
        ("compare.file", 0x55AA66),
    ]
    .map(|(role, tint)| {
        let mix = |amount: u32| {
            [16, 8, 0].into_iter().fold(0, |rgb, shift| {
                rgb | (((((background >> shift) & 255) * (100 - amount)
                    + ((tint >> shift) & 255) * amount
                    + 50)
                    / 100)
                    << shift)
            })
        };
        let amount = (0..=25)
            .rev()
            .find(|amount| contrast(mix(*amount), text) >= 4.5)
            .unwrap_or(0);
        (role, mix(amount))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn current_search_hit_uses_readable_selection_without_changing_rgb_ui_skin() {
        for theme in catalog().unwrap() {
            let cfg = config(&theme.id, Path::new("x"), Path::new("y")).unwrap();
            let color = |role: &str| u32::from_str_radix(&theme.colors[role][1..], 16).unwrap();
            let target = |kind| {
                cfg.maps[..cfg.map_count as usize]
                    .iter()
                    .find(|m| m.kind == kind && m.source == 0xF3EECD)
                    .unwrap()
                    .target
            };
            assert_eq!(target(6), color("selection.background"), "{}", theme.id);
            assert!(
                contrast(target(6), color("selection.text")) >= 4.5,
                "{}",
                theme.id
            );
            // Search also paints ordinary identifier ink, not only inverse glyphs.
            assert!(contrast(target(6), color("text")) >= 4.5, "{}", theme.id);
            assert_eq!(target(5), color("panel"), "{}", theme.id);
        }
    }
    #[test]
    fn managed_form_chrome_and_links_are_readable_in_every_theme() {
        for theme in catalog().unwrap() {
            let cfg = config(&theme.id, Path::new("x"), Path::new("y")).unwrap();
            let color = |role: &str| u32::from_str_radix(&theme.colors[role][1..], 16).unwrap();
            for kind in [5, 6] {
                let target = |source| {
                    cfg.maps[..cfg.map_count as usize]
                        .iter()
                        .find(|m| m.kind == kind && m.source == source)
                        .unwrap()
                        .target
                };
                for source in [0xEBEBEB, 0xF2F2F2] {
                    assert_eq!(target(source), color("panel"), "{}", theme.id);
                    assert!(
                        contrast(target(source), color("text")) >= 4.5,
                        "{}",
                        theme.id
                    );
                }
                assert_eq!(target(0xB3B3B3), color("border"));
                for background in [color("panel"), color("editor.background")] {
                    assert!(
                        contrast(target(0x1C55AE), background) >= 4.5,
                        "{}",
                        theme.id
                    );
                }
            }
        }
    }
    #[test]
    fn preview_catalog_matches_native_palettes() {
        let catalog = catalog().unwrap();
        assert_eq!(catalog.len(), 13);
        assert_eq!(catalog.iter().filter(|t| t.dark).count(), 11);
        for preview in catalog {
            let cfg = config(&preview.id, Path::new("x"), Path::new("y")).unwrap();
            let text = u32::from_str_radix(&preview.colors["text"][1..], 16).unwrap();
            assert_eq!(cfg.colors[1], text);
        }
    }
    #[test]
    fn editor_preview_and_native_syntax_use_the_same_mapping() {
        for preview in catalog().unwrap() {
            let cfg = config(&preview.id, Path::new("x"), Path::new("y")).unwrap();
            for kind in [1, 6] {
                let identifier = cfg.maps[..cfg.map_count as usize]
                    .iter()
                    .find(|m| m.kind == kind && m.source == 0x0000FF)
                    .unwrap();
                assert_eq!(
                    format!("#{:06X}", identifier.target),
                    preview.colors["text"]
                );
            }
            assert_eq!(preview.editor["identifier"], preview.colors["text"]);
            assert_eq!(preview.editor["builtin"], preview.colors["text"]);
            assert_eq!(preview.editor["keyword"], preview.colors["keyword"]);
            assert_eq!(preview.editor["punctuation"], preview.colors["keyword"]);
            assert_eq!(preview.editor["string"], preview.colors["text"]);
            assert_eq!(preview.editor["number"], preview.colors["text"]);
            assert_eq!(preview.editor["comment"], preview.colors["comment"]);
            assert_eq!(preview.editor["directive"], preview.colors["directive"]);
        }
    }
    #[test]
    fn all_presets_match_protocol() {
        for id in IDS.iter().chain(std::iter::once(&"diagnostic")) {
            let cfg = config(
                id,
                Path::new(r"C:\Platform"),
                Path::new(r"C:\Logs\colors.csv"),
            )
            .unwrap();
            assert_eq!(cfg.bytes().len(), 4760);
            assert_eq!(cfg.map_count, if *id == "diagnostic" { 0 } else { 201 });
        }
    }
    #[test]
    fn comparison_statuses_are_readable_distinct_and_match_the_legend() {
        for id in IDS {
            let cfg = config(id, Path::new("x"), Path::new("y")).unwrap();
            let target = |kind, source| {
                cfg.maps[..cfg.map_count as usize]
                    .iter()
                    .find(|m| m.kind == kind && m.source == source)
                    .unwrap()
                    .target
            };
            assert_eq!(target(6, 0x0000A0), cfg.colors[1], "module text {id}");
            let statuses = [0xE0FFFF, 0xFFE4C4, 0xC0DCC0].map(|source| target(5, source));
            for source in [0xE0FFFF, 0xFFE4C4, 0xC0DCC0, 0xBFF7B8] {
                assert_eq!(target(5, source), target(6, source), "RGB/RGBA {id}");
                assert!(
                    contrast(target(5, source), cfg.colors[1]) >= 4.5,
                    "text {id}"
                );
            }
            assert_eq!(target(5, 0xC0DCC0), target(5, 0xBFF7B8), "legend {id}");
            for i in 0..3 {
                for j in i + 1..3 {
                    let difference: i32 = [16, 8, 0]
                        .iter()
                        .map(|shift| {
                            (((statuses[i] >> shift) & 255) as i32
                                - ((statuses[j] >> shift) & 255) as i32)
                                .abs()
                        })
                        .sum();
                    assert!(difference >= 30, "statuses blend together: {id}");
                }
            }
            let changed_code = target(5, 0xF5F5F5);
            assert_eq!(changed_code, statuses[0], "changed code block {id}");
            assert_ne!(changed_code, cfg.colors[0], "difference erased {id}");
            // This ink is the native correction's fallback. Actual syntax inks
            // are covered by contrast_test and the production Cairo glyph test.
            assert!(contrast(changed_code, cfg.colors[1]) >= 4.5, "code {id}");
            assert_eq!(target(6, 0xF5F5F5), cfg.colors[4], "RGBA UI skin {id}");
        }
        assert_eq!(
            config("diagnostic", Path::new("x"), Path::new("y"))
                .unwrap()
                .map_count,
            0
        );
    }
    #[test]
    fn rejects_unknown_and_long_paths() {
        assert!(config("../dracula", Path::new("x"), Path::new("y")).is_err());
        assert!(config("dracula", Path::new(&"x".repeat(260)), Path::new("y")).is_err());
    }
    #[test]
    fn icon_modes_match_independent_goldens() {
        for (id, enabled, fixture) in [
            ("dracula", true, "icons-dark"),
            ("github-light-default", true, "icons-light"),
            ("standard", true, "icons-standard"),
            ("diagnostic", true, "diagnostic"),
        ] {
            let cfg = config_with_icons(
                id,
                Path::new(r"C:\Platform"),
                Path::new(r"C:\Logs\colors.csv"),
                enabled,
            )
            .unwrap();
            let expected = std::fs::read(
                Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("tests/golden")
                    .join(format!("{fixture}.bin")),
            )
            .unwrap();
            assert_eq!(cfg.bytes(), expected);
        }
    }
    #[test]
    fn binary_protocol_matches_independent_python_fixtures() {
        for id in IDS {
            let cfg = config(
                id,
                Path::new(r"C:\Platform"),
                Path::new(r"C:\Logs\colors.csv"),
            )
            .unwrap();
            let expected = std::fs::read(
                Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("tests/golden")
                    .join(format!("{id}.bin")),
            )
            .unwrap();
            assert_eq!(cfg.bytes(), expected, "{id}");
        }
    }
}
