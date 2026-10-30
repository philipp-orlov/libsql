"""Pictures for the demo reports, made without libraries: a PNG banner (the report's cover) and the SVG logo."""
import base64
import math
import struct
import zlib


def png(width, height, pixel):
    """A PNG (8-bit RGB) whose pixel (x, y) is pixel(x, y) -> (r, g, b); rows are filtered with Sub so smooth pictures shrink."""
    rows = []
    for y in range(height):
        row = [pixel(x, y) for x in range(width)]
        out = bytearray([1])
        prev = (0, 0, 0)
        for p in row:
            out += bytes((p[i] - prev[i]) & 255 for i in range(3))
            prev = p
        rows.append(bytes(out))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def banner(width=1000, height=96, left=(13, 71, 161), right=(0, 137, 123)):
    """A gradient from `left` to `right` with soft rings and a rising line of dots, like a chart seen from far away."""
    rings = [(0.18, 1.1, 80, 0.10), (0.18, 1.1, 120, 0.07), (0.78, -0.2, 90, 0.10), (0.78, -0.2, 140, 0.06), (0.95, 1.2, 70, 0.08)]
    points = [(0.52 + 0.065 * i, 0.82 - 0.11 * i - 0.07 * math.sin(i * 1.7)) for i in range(7)]
    pts = [(x * width, y * height) for x, y in points]

    def seg(px, py, a, b):
        ax, ay = a
        bx, by = b
        dx, dy = bx - ax, by - ay
        t = max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(px - (ax + t * dx), py - (ay + t * dy))

    def pixel(x, y):
        c = mix(left, right, x / (width - 1))
        c = mix(c, (0, 0, 0), 0.10 * y / height)  # a little darker toward the bottom
        for cx, cy, r, a in rings:
            d = abs(math.hypot(x - cx * width, y - cy * height) - r)
            if d < 1.6:
                c = mix(c, (255, 255, 255), a * (1 - d / 1.6) * 2.2)
        d = min(seg(x, y, pts[i], pts[i + 1]) for i in range(len(pts) - 1))
        if d < 1.4:
            c = mix(c, (255, 255, 255), 0.55 * (1 - d / 1.4))
        for px, py in pts:
            r = math.hypot(x - px, y - py)
            if r < 4.2:
                c = mix(c, (255, 255, 255), 0.9 if r < 3 else 0.9 * (4.2 - r) / 1.2)
            elif r < 7:
                c = mix(c, (255, 255, 255), 0.18 * (7 - r) / 2.8)
        return tuple(int(round(v)) for v in c)

    return png(width, height, pixel)


def data_uri(mime, data):
    return f'data:{mime};base64,' + base64.b64encode(data).decode()


# ---- the demo database's pictures: category icons and the company logo (SVG, no text, so they look the same everywhere) ----
def svg_uri(body, size=64):
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" width="{size}" height="{size}">{body}</svg>'
    return 'data:image/svg+xml;base64,' + base64.b64encode(svg.encode()).decode()


def tile(color, glyph):
    return f'<rect width="64" height="64" rx="14" fill="{color}"/><g fill="none" stroke="#fff" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round">{glyph}</g>'


ICONS = {
    # a rack of three servers
    'Compute': ('#4e79a7', '<rect x="14" y="14" width="36" height="10" rx="3"/><rect x="14" y="27" width="36" height="10" rx="3"/><rect x="14" y="40" width="36" height="10" rx="3"/>'
                '<circle cx="21" cy="19" r="1.2" fill="#fff"/><circle cx="21" cy="32" r="1.2" fill="#fff"/><circle cx="21" cy="45" r="1.2" fill="#fff"/>'),
    # a hub with three nodes
    'Networking': ('#f28e2b', '<circle cx="32" cy="32" r="6" fill="#fff"/><circle cx="14" cy="16" r="4"/><circle cx="50" cy="16" r="4"/><circle cx="32" cy="52" r="4"/>'
                   '<path d="M18 19l9 9M46 19l-9 9M32 38v10"/>'),
    # a database cylinder
    'Storage': ('#e15759', '<ellipse cx="32" cy="17" rx="15" ry="6"/><path d="M17 17v30c0 3.3 6.7 6 15 6s15-2.7 15-6V17"/><path d="M17 32c0 3.3 6.7 6 15 6s15-2.7 15-6"/>'),
    # angle brackets and a slash
    'Software': ('#76b7b2', '<path d="M24 20L12 32l12 12M40 20l12 12-12 12M35 16l-6 32"/>'),
    # a lifebuoy
    'Services': ('#59a14f', '<circle cx="32" cy="32" r="18"/><circle cx="32" cy="32" r="7"/><path d="M19.3 19.3l7.8 7.8M44.7 19.3l-7.8 7.8M19.3 44.7l7.8-7.8M44.7 44.7l-7.8-7.8"/>'),
    # a mouse
    'Accessories': ('#b07aa1', '<rect x="20" y="10" width="24" height="44" rx="12"/><path d="M32 10v16M20 26h24"/>'),
}
TAGLINES = {
    'Compute': 'Rack and tower servers, laptops and desktops',
    'Networking': 'Switches, access points, firewalls and SD-WAN',
    'Storage': 'NAS, arrays, backup appliances and tape',
    'Software': 'Analytics, security, CI/CD and monitoring, per seat',
    'Services': 'Support, installation, audits, migrations and training',
    'Accessories': 'Keyboards, monitors, docks, racks and power',
}

# the company's mark: a hexagon around a peak and a rising line
LOGO = svg_uri(
    '<polygon points="32,3 58,18 58,46 32,61 6,46 6,18" fill="#0d47a1"/>'
    '<path d="M14 44l13-18 8 10 5-7 10 15z" fill="#ffffff"/><path d="M14 44l13-18 8 10" fill="none" stroke="#fb8c00" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/>')
