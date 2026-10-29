"""Rasterize project-owned SVG masters and fingerprint synthetic test surfaces."""
import struct
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

SIZES = (14, 16, 20, 24, 28, 32, 48, 64)


def raster(vector, size):
    scale = size * 4 / 16
    root = ET.fromstring(vector)
    image = Image.new("RGBA", (size*4, size*4))
    draw = ImageDraw.Draw(image)
    color = root.attrib["stroke"]
    stroke = max(1, round(float(root.attrib["stroke-width"]) * scale))
    for element in root:
        kind = element.tag.split("}")[-1]
        values = element.attrib
        def number(key):
            return float(values[key]) * scale
        if kind in ("polyline", "polygon"):
            points = [tuple(float(n)*scale for n in point.split(",")) for point in values["points"].split()]
            if kind == "polygon":
                points.append(points[0])
            draw.line(points, fill=color, width=stroke, joint="curve")
        elif kind == "line":
            points = [(number("x1"), number("y1")), (number("x2"), number("y2"))]
            draw.line(points, fill=color, width=stroke)
        elif kind == "rect":
            x, y = number("x"), number("y")
            draw.rectangle((x, y, x+number("width"), y+number("height")), outline=color, width=stroke)
            points = []
        elif kind == "ellipse":
            x, y, rx, ry = number("cx"), number("cy"), number("rx"), number("ry")
            draw.ellipse((x-rx,y-ry,x+rx,y+ry), outline=color, width=stroke)
            points = []
        else:
            raise ValueError(kind)
        for x, y in points:
            r = stroke/2
            draw.ellipse((x-r,y-r,x+r,y+r), fill=color)
    return image.resize((size,size), Image.Resampling.LANCZOS)


def fingerprint(pixels, width, height):
    value = 14695981039346656037
    for byte in struct.pack("<II",width,height) + pixels:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


