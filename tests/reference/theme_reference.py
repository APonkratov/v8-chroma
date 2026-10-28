"""Versioned theme files and platform color profiles. No Windows dependencies."""
from dataclasses import dataclass
import json
from pathlib import Path
import re

KINDS = {'text': 1, 'background': 2, 'brush': 3, 'pen': 4,
         'cairo-rgb': 5, 'cairo-rgba': 6, 'color-stop': 7, 'mesh': 8}
ROLES = ('editor.background', 'text', 'panel', 'border', 'selection.background',
         'selection.text', 'line', 'comment', 'keyword', 'string', 'number',
         'directive', 'accent', 'error', 'warning', 'disabled', 'hover')
# Order is part of protocol v2 (RenderColors in src/protocol.h).
BINDINGS = ('glyph.background', 'glyph.foreground', 'glyph.selection.background',
            'glyph.selection.foreground', 'raster.background', 'raster.foreground',
            'window.caption', 'window.text', 'window.border')
ID = re.compile(r'[a-z0-9][a-z0-9-]{0,62}')
RGB = re.compile(r'#[0-9a-fA-F]{6}')


def exact_keys(value, required, location):
    if not isinstance(value, dict):
        raise ValueError(f'{location}: expected an object')
    missing, unknown = set(required)-value.keys(), value.keys()-set(required)
    if missing or unknown:
        raise ValueError(f'{location}: missing {sorted(missing)}, unknown {sorted(unknown)}')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'Duplicate JSON key: {key}')
        result[key] = value
    return result


@dataclass(frozen=True)
class Theme:
    id: str
    name: str
    colors: dict
    render: dict
    source: bytes

    @property
    def native_colors(self):
        return tuple(self.colors[self.render[key]] for key in BINDINGS) + (int(self.render['window.dark']),)


def load_theme(path):
    with Path(path).open('rb') as stream:
        data = stream.read(65537)
    if len(data) > 65536:
        raise ValueError('Theme file exceeds 64 KiB')
    document = json.loads(data.decode('utf-8-sig'), object_pairs_hook=unique_object)
    exact_keys(document, ('format_version', 'id', 'name', 'colors', 'render'), 'theme')
    if type(document['format_version']) is not int or document['format_version'] != 1:
        raise ValueError('Unsupported theme format_version; expected 1')
    identifier, name = document['id'], document['name']
    if not isinstance(identifier, str) or not ID.fullmatch(identifier) or identifier == 'diagnostic':
        raise ValueError('Theme id: use 1-63 lowercase letters, digits or hyphens; diagnostic is reserved')
    if not isinstance(name, str) or not name.strip() or len(name) > 80 or not name.isprintable():
        raise ValueError('Theme name: expected 1-80 printable characters')
    colors, render = document['colors'], document['render']
    exact_keys(colors, ROLES, 'colors')
    for key, value in colors.items():
        if not isinstance(value, str) or not RGB.fullmatch(value):
            raise ValueError(f'colors.{key}: expected #RRGGBB')
    exact_keys(render, (*BINDINGS, 'window.dark'), 'render')
    for key in BINDINGS:
        if not isinstance(render[key], str) or render[key] not in colors:
            raise ValueError(f'render.{key}: unknown color role {render[key]!r}')
    if type(render['window.dark']) is not bool:
        raise ValueError('render.window.dark: expected true or false')
    values = {key: int(value[1:], 16) for key, value in colors.items()}
    if (values[render['glyph.background']] == values[render['glyph.selection.background']] and
            values[render['glyph.foreground']] != values[render['glyph.selection.foreground']]):
        raise ValueError('Glyph background roles coincide but foreground roles differ; rendering would be ambiguous')
    return Theme(identifier, name, values, dict(render), data)


def load_profile(path, theme):
    result, seen = [], set()
    for number, raw in enumerate(Path(path).read_text(encoding='utf-8-sig').splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        key, separator, role = line.partition('=')
        kind, dot, source = key.strip().partition('.')
        role = role.strip()
        if (not separator or not dot or kind not in KINDS or
                not re.fullmatch(r'[0-9a-fA-F]{6}', source)):
            raise ValueError(f'Profile line {number}: invalid mapping')
        identity = (KINDS[kind], int(source, 16))
        if identity in seen:
            raise ValueError(f'Profile line {number}: duplicate source mapping')
        if role not in theme.colors:
            raise ValueError(f'Profile line {number}: unknown role {role!r}')
        seen.add(identity)
        result.append((*identity, theme.colors[role]))
    if not result or len(result)>256:
        raise ValueError('Profile must have 1-256 mappings')
    return result
