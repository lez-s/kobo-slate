#!/usr/bin/env python3
"""Convert a Kobo notepad notebook (.knp) to SVG, and optionally to PDF.

The notebook format is append-only binary; see kobo/notepad/README.md. This is
the reference decoder as well as the escape hatch: everything the device stores
can be recovered here without the device.

    python3 knp2svg.py canvas.knp                 -> canvas.svg
    python3 knp2svg.py canvas.knp -o out.svg
    python3 knp2svg.py canvas.knp --pdf           -> canvas.svg + canvas.pdf

PDF goes through headless Chrome, so there is nothing to install beyond a
browser you already have. Override the binary with CHROME_PATH.
"""

import argparse
import os
import pathlib
import struct
import subprocess
import sys

MAGIC = b"KNP1"
OP_STROKE = 0x01
OP_ERASE = 0x02

# Must match NP_PIXEL_* in notepad.h, in the same order as the NPColor enum.
COLORS = ["#000000", "#cc0000", "#0033cc", "#007a1f"]
COLOR_NAMES = ["black", "red", "blue", "green"]

CHROME_DEFAULT = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


class BadNotebook(Exception):
    pass


def load(path):
    """Return (version, [stroke]) with erases applied, tolerating a torn tail.

    A stroke is (id, tool, width, color, [(x, y), ...]). The device appends one
    record per pen-up, so a battery-out mid-append leaves a partial final
    record; the loader keeps everything before it rather than rejecting the
    file, exactly as the device does.
    """
    data = path.read_bytes()
    if len(data) < 6 or data[:4] != MAGIC:
        raise BadNotebook(f"{path}: not a KNP notebook")
    version = struct.unpack_from("<H", data, 4)[0]
    if version not in (1, 2):
        raise BadNotebook(f"{path}: unsupported format version {version}")

    # v1 has no colour byte; every v1 stroke is black.
    head = 3 if version == 1 else 4  # opcode + tool + width [+ color]
    offset, order, live, truncated = 6, [], {}, 0

    while offset < len(data):
        opcode = data[offset]
        if opcode == OP_STROKE:
            if offset + head + 8 > len(data):
                truncated += 1
                break
            tool = data[offset + 1]
            width = data[offset + 2]
            color = data[offset + 3] if version == 2 else 0
            sid, npts = struct.unpack_from("<II", data, offset + head)
            body = offset + head + 8
            if body + npts * 8 > len(data):
                truncated += 1
                break
            coords = struct.unpack_from("<%df" % (npts * 2), data, body)
            if sid not in live:
                order.append(sid)
            live[sid] = (sid, tool, width, color,
                         list(zip(coords[0::2], coords[1::2])))
            offset = body + npts * 8
        elif opcode == OP_ERASE:
            if offset + 5 > len(data):
                truncated += 1
                break
            sid = struct.unpack_from("<I", data, offset + 1)[0]
            live.pop(sid, None)
            offset += 5
        else:
            raise BadNotebook(
                f"{path}: unknown opcode {opcode:#04x} at byte {offset}")

    if truncated:
        print(f"note: discarded a truncated final record in {path.name}",
              file=sys.stderr)
    return version, [live[i] for i in order if i in live]


def to_svg(strokes, margin=40):
    """Render to SVG, cropped to the drawn content plus a margin.

    The canvas is infinite, so there is no page to fit: the bounding box of the
    ink is the only sensible extent.
    """
    if not strokes:
        return '<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0"/>\n'

    xs = [x for _, _, _, _, pts in strokes for x, _ in pts]
    ys = [y for _, _, _, _, pts in strokes for _, y in pts]
    min_x, max_x = min(xs) - margin, max(xs) + margin
    min_y, max_y = min(ys) - margin, max(ys) + margin
    width, height = max_x - min_x, max_y - min_y

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{width:.0f}" height="{height:.0f}" '
        f'viewBox="{min_x:.2f} {min_y:.2f} {width:.2f} {height:.2f}">',
        '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="#ffffff"/>'
        % (min_x, min_y, width, height),
    ]
    for _, _, stroke_width, color, pts in strokes:
        stroke = COLORS[color] if color < len(COLORS) else COLORS[0]
        if len(pts) == 1:
            x, y = pts[0]
            out.append('<circle cx="%.2f" cy="%.2f" r="%.2f" fill="%s"/>'
                       % (x, y, stroke_width / 2.0, stroke))
            continue
        points = " ".join("%.2f,%.2f" % (x, y) for x, y in pts)
        out.append(
            '<polyline points="%s" fill="none" stroke="%s" stroke-width="%d" '
            'stroke-linecap="round" stroke-linejoin="round"/>'
            % (points, stroke, stroke_width))
    out.append("</svg>")
    return "\n".join(out) + "\n"


def svg_dimensions(svg, fallback=(1024, 1024)):
    """Pull width/height back out of the SVG root element."""
    import re
    match = re.search(r'<svg[^>]*?width="(\d+)"[^>]*?height="(\d+)"', svg)
    if not match:
        return fallback
    return int(match.group(1)), int(match.group(2))


def to_pdf(svg_path, pdf_path, timeout=60):
    chrome = os.environ.get("CHROME_PATH", CHROME_DEFAULT)
    # Both must be absolute: as_uri() rejects a relative path outright, and
    # Chrome would resolve a relative --print-to-pdf against its own cwd.
    svg_path = svg_path.resolve()
    pdf_path = pdf_path.resolve()

    # Printing the SVG directly gives whatever paper size Chrome defaults to,
    # which paginates a tall drawing across several sheets. An infinite canvas
    # has no natural page, so make the page exactly the drawing: one sheet, no
    # margins, nothing cropped. The SVG is inlined so there is nothing to
    # resolve relative to the temporary file.
    svg = svg_path.read_text()
    width, height = svg_dimensions(svg)
    html_path = svg_path.with_suffix(".print.html")
    html_path.write_text(
        "<!doctype html><meta charset=utf-8>"
        "<style>@page{size:%dpx %dpx;margin:0}"
        "html,body{margin:0;padding:0}svg{display:block}</style>%s"
        % (width, height, svg))

    command = [
        chrome, "--headless", "--disable-gpu", "--no-pdf-header-footer",
        f"--print-to-pdf={pdf_path}", html_path.as_uri(),
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=timeout)
    except FileNotFoundError as exc:
        raise RuntimeError(f"Chrome binary not found: {chrome}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"Chrome timed out after {timeout}s") from exc
    finally:
        html_path.unlink(missing_ok=True)
    if not pdf_path.exists():
        raise RuntimeError(
            f"Chrome produced no PDF (exit {result.returncode}): "
            f"{result.stderr.strip()[:400]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("notebook", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path,
                        help="SVG path (default: alongside the notebook)")
    parser.add_argument("--pdf", action="store_true",
                        help="also render a PDF through headless Chrome")
    parser.add_argument("--margin", type=float, default=40.0)
    args = parser.parse_args()

    try:
        version, strokes = load(args.notebook)
    except BadNotebook as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    svg_path = args.output or args.notebook.with_suffix(".svg")
    svg_path.write_text(to_svg(strokes, args.margin))
    points = sum(len(s[4]) for s in strokes)
    used = sorted({s[3] for s in strokes})
    print(f"{args.notebook.name}: v{version}, {len(strokes)} strokes, "
          f"{points} points, colours {[COLOR_NAMES[c] for c in used]}")
    print(f"wrote {svg_path}")

    if args.pdf:
        pdf_path = svg_path.with_suffix(".pdf")
        try:
            to_pdf(svg_path, pdf_path)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        print(f"wrote {pdf_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
