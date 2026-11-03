"""Check native shaping and antialiasing without 1C or redistributed fonts.

Use --cairo /opt/.../libcairo-v8.so to check the user's complete bundled stack.
Every object stays within its own Cairo/FreeType/HarfBuzz provider.
"""
import argparse
import ctypes as c
import ctypes.util
import json
import math
import os
from pathlib import Path
import subprocess


class Feature(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in ("tag", "value", "start", "end")]


class Info(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in ("codepoint", "mask", "cluster", "var1", "var2")]


class Position(c.Structure):
    _fields_ = [(name, c.c_int32) for name in ("x_advance", "y_advance", "x_offset", "y_offset", "var")]


class Matrix(c.Structure):
    _fields_ = [(name, c.c_double) for name in ("xx", "yx", "xy", "yy", "x0", "y0")]


class Glyph(c.Structure):
    _fields_ = [("index", c.c_ulong), ("x", c.c_double), ("y", c.c_double)]


def function(library, name, result, *arguments):
    fn = getattr(library, name)
    fn.restype, fn.argtypes = result, arguments
    return fn


def tag(value):
    return int.from_bytes(value.encode("ascii"), "big")


class Stack:
    def __init__(self, cairo=None):
        mode = os.RTLD_LOCAL | os.RTLD_NOW | getattr(os, "RTLD_DEEPBIND", 0)
        # 1C's bundled Cairo expects X11 exports supplied by its host.
        self.dependencies = [c.CDLL(ctypes.util.find_library(name), mode=os.RTLD_GLOBAL | os.RTLD_NOW)
                             for name in ("X11", "Xext", "Xrender")] if cairo else []
        self.cairo = c.CDLL(cairo or ctypes.util.find_library("cairo"), mode=mode)
        self.ft = self.cairo if cairo else c.CDLL(ctypes.util.find_library("freetype"), mode=mode)
        self.hb = self.cairo if cairo else c.CDLL(ctypes.util.find_library("harfbuzz"), mode=mode)
        self.init = function(self.ft, "FT_Init_FreeType", c.c_int, c.POINTER(c.c_void_p))
        self.done = function(self.ft, "FT_Done_FreeType", c.c_int, c.c_void_p)
        self.new_face = function(self.ft, "FT_New_Face", c.c_int, c.c_void_p, c.c_char_p, c.c_long, c.POINTER(c.c_void_p))
        self.done_face = function(self.ft, "FT_Done_Face", c.c_int, c.c_void_p)
        self.size = function(self.ft, "FT_Set_Pixel_Sizes", c.c_int, c.c_void_p, c.c_uint, c.c_uint)
        self.font = function(self.hb, "hb_ft_font_create_referenced", c.c_void_p, c.c_void_p)
        self.font_destroy = function(self.hb, "hb_font_destroy", None, c.c_void_p)
        self.buffer = function(self.hb, "hb_buffer_create", c.c_void_p)
        self.buffer_destroy = function(self.hb, "hb_buffer_destroy", None, c.c_void_p)
        self.add = function(self.hb, "hb_buffer_add_utf16", None, c.c_void_p, c.POINTER(c.c_uint16), c.c_int, c.c_uint, c.c_int)
        self.direction = function(self.hb, "hb_buffer_set_direction", None, c.c_void_p, c.c_int)
        self.script = function(self.hb, "hb_buffer_set_script", None, c.c_void_p, c.c_uint32)
        self.shape = function(self.hb, "hb_shape", None, c.c_void_p, c.c_void_p, c.POINTER(Feature), c.c_uint)
        self.infos = function(self.hb, "hb_buffer_get_glyph_infos", c.POINTER(Info), c.c_void_p, c.POINTER(c.c_uint))
        self.positions = function(self.hb, "hb_buffer_get_glyph_positions", c.POINTER(Position), c.c_void_p, c.POINTER(c.c_uint))
        self.library = c.c_void_p()
        assert self.init(c.byref(self.library)) == 0

    def close(self):
        assert self.done(self.library) == 0

    def open(self, file, index, pixels):
        face = c.c_void_p()
        assert self.new_face(self.library, os.fsencode(file), index, c.byref(face)) == 0
        assert self.size(face, 0, pixels) == 0
        return face

    def shaped(self, font, text, features=None, rtl=False):
        data = text.encode("utf-16-le")
        units = (c.c_uint16 * (len(data) // 2)).from_buffer_copy(data)
        buffer = self.buffer()
        try:
            self.add(buffer, units, len(units), 0, len(units))
            # These are the Common/LTR operator runs observed in grphcs.so.
            self.direction(buffer, 5 if rtl else 4)
            self.script(buffer, tag("Hebr" if rtl else "Zyyy"))
            records = None if features is None else (Feature * len(features))(*features)
            self.shape(font, buffer, records, 0 if features is None else len(features))
            count = c.c_uint()
            info = self.infos(buffer, c.byref(count))
            positions = self.positions(buffer, c.byref(count))
            return [(info[i].codepoint, info[i].cluster, positions[i].x_advance,
                     positions[i].y_advance, positions[i].x_offset, positions[i].y_offset)
                    for i in range(count.value)]
        finally:
            self.buffer_destroy(buffer)


def carets(glyphs, length):
    """Character cells inside a multi-character cluster, in 26.6 units."""
    groups = {}
    for _, cluster, advance, *_ in glyphs:
        groups[cluster] = groups.get(cluster, 0) + advance
    result, x = [None] * (length + 1), 0
    starts = sorted(groups)
    for start, end in zip(starts, starts[1:] + [length]):
        assert 0 <= start < end <= length
        for position in range(start, end):
            result[position] = x + groups[start] * (position - start) / (end - start)
        x += groups[start]
    result[length] = x
    return result


def selected_font(family):
    output = subprocess.check_output(["fc-match", "--format=%{family}\n%{file}\n%{index}\n", family], text=True).splitlines()
    assert family.casefold() in (name.strip().casefold() for name in output[0].split(",")), f"Missing font {family}: {output[0]}"
    return output[1], int(output[2])


def raster(stack, face, glyphs, pixels, scale, antialias, dark, output=None):
    lib = stack.cairo
    def fn(name, result, *args):
        return function(lib, "cairo_" + name, result, *args)
    pointer, number = c.c_void_p, c.c_int
    surface = fn("image_surface_create", pointer, number, number, number)(1, 1100, 110)
    context = fn("create", pointer, pointer)(surface)
    font_face = fn("ft_font_face_create_for_ft_face", pointer, pointer, number)(face, 0)
    options = fn("font_options_create", pointer)()
    scaled = None
    try:
        fn("font_options_set_antialias", None, pointer, number)(options, antialias)
        fn("font_options_set_subpixel_order", None, pointer, number)(options, 1)
        fn("font_options_set_hint_style", None, pointer, number)(options, 2)
        matrix, ctm = Matrix(pixels, 0, 0, pixels, 0, 0), Matrix(scale, 0, 0, scale, 0, 0)
        scaled = fn("scaled_font_create", pointer, pointer, c.POINTER(Matrix), c.POINTER(Matrix), pointer)(font_face, c.byref(matrix), c.byref(ctm), options)
        assert fn("scaled_font_status", number, pointer)(scaled) == 0
        fn("scale", None, pointer, c.c_double, c.c_double)(context, scale, scale)
        fn("set_scaled_font", None, pointer, pointer)(context, scaled)
        color = fn("set_source_rgb", None, pointer, c.c_double, c.c_double, c.c_double)
        color(context, *(0, 0, 0) if dark else (1, 1, 1))
        fn("paint", None, pointer)(context)
        color(context, *(1, 1, 1) if dark else (0, 0, 0))
        x, y = 4.0, float(pixels + 4)
        positions = []
        for index, _, advance, vertical, dx, dy in glyphs:
            positions.append(Glyph(index, x + dx / 64, y - dy / 64))
            x += advance / 64
            y -= vertical / 64
        array = (Glyph * len(positions))(*positions)
        before = bytes(array)
        fn("show_glyphs", None, pointer, c.POINTER(Glyph), number)(context, array, len(array))
        assert bytes(array) == before, "Renderer mutated glyph positions"
        assert fn("status", number, pointer)(context) == 0
        fn("surface_flush", None, pointer)(surface)
        stride = fn("image_surface_get_stride", number, pointer)(surface)
        data = fn("image_surface_get_data", c.POINTER(c.c_ubyte), pointer)(surface)
        raw = c.string_at(data, stride * 110)
        # Native-endian RGB24 is BGRx on supported Linux x64.
        gray = colored = 0
        for offset in range(0, len(raw), 4):
            b, g, r = raw[offset:offset + 3]
            gray += any(0 < channel < 255 for channel in (r, g, b))
            colored += r != g or g != b
        if antialias == 1:
            assert gray == colored == 0, "NONE must be a binary coverage control"
        else:
            assert gray > 0, "Missing antialias coverage"
            assert colored == 0 if antialias == 2 else colored > 0, "GRAY/SUBPIXEL coverage mismatch"
        if output:
            assert fn("surface_write_to_png", number, pointer, c.c_char_p)(surface, os.fsencode(output)) == 0
        return gray, colored
    finally:
        if scaled:
            fn("scaled_font_destroy", None, pointer)(scaled)
        fn("font_options_destroy", None, pointer)(options)
        fn("font_face_destroy", None, pointer)(font_face)
        fn("destroy", None, pointer)(context)
        fn("surface_destroy", None, pointer)(surface)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cairo")
    parser.add_argument("--family", default="DejaVu Sans Mono")
    parser.add_argument("--require-ligatures", action="store_true")
    parser.add_argument("--expect-no-ligatures", action="store_true")
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    file, index = selected_font(args.family)
    stack = Stack(args.cairo)
    off = [Feature(tag(name), 0, 0, 0xffffffff) for name in ("calt", "liga")]
    on = [Feature(tag(name), 1, 0, 0xffffffff) for name in ("calt", "liga")]
    operators = ["<=", ">=", "<>", "!=", "==", "=>", "->", ":=", "//"]
    samples = operators + ["Если А <= Б И Б >= А И А <> Б Тогда", 'Сообщить("<= >= <> != == => -> := //");',
                           "Latin <= Кириллица -> end", "\t\tЕсли А <= Б Тогда", "ЁёЙйДдЩщ 0123456789", "iiiWWW <= != =>"]
    substitutions = checks = raster_checks = 0
    if args.evidence:
        args.evidence.mkdir(parents=True, exist_ok=True)
    try:
        for points10 in (130, 195, 200):
            # Match the platform/Windows MulDiv point conversion at 96 DPI.
            pixels = (points10 * 96 + 360) // 720
            face = stack.open(file, index, pixels)
            font = stack.font(face)
            try:
                for sample in samples:
                    native = stack.shaped(font, sample)
                    enabled = stack.shaped(font, sample, on)
                    disabled = stack.shaped(font, sample, off)
                    assert native == enabled, f"Native ligatures disabled: {sample!r}"
                    if sample in operators:
                        assert all(glyph[0] for glyph in native), "Missing operator glyph"
                        substitutions += native != disabled
                    # Tabs are positioning controls handled by the editor;
                    # only compare native/on there, not invented tab glyph cells.
                    if "\t" not in sample:
                        n, d = carets(native, len(sample)), carets(disabled, len(sample))
                        assert len(n) == len(d) and all(a is not None and b is not None and math.isclose(a, b, abs_tol=1) for a, b in zip(n, d)), f"Changed character geometry: {sample!r}"
                    checks += 1
                text = stack.shaped(font, "Проверка ABCD <= != -> 0123")
                for scale in (1, 1.25, 1.5, 2):
                    for dark in (False, True):
                        for aa, label in ((1, "none"), (2, "gray"), (3, "subpixel")):
                            output = args.evidence / f"{points10}-{scale}-{int(dark)}-{label}.png" if args.evidence and points10 == 130 and scale == 1 else None
                            raster(stack, face, text, pixels, scale, aa, dark, output)
                            raster_checks += 1
            finally:
                stack.font_destroy(font)
                assert stack.done_face(face) == 0
        assert not args.require_ligatures or substitutions > 0, "Positive ligature font did not substitute any operator"
        assert not args.expect_no_ligatures or substitutions == 0, "Unexpected programming ligatures"
    finally:
        stack.close()
    print(json.dumps({"family": args.family, "provider": args.cairo or "system", "shaping_checks": checks,
                      "operator_substitutions": substitutions, "raster_checks": raster_checks, "result": "PASS"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
