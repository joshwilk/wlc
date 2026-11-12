#!/usr/bin/env python3
"""Draws the WLC traffic-barrel artwork and writes every Mac icon from it.

    python3 tools-wlc/make_icons.py        (needs rsvg-convert and Pillow)

Outputs: tools-wlc/art/barrel.svg (master), the app icons, the document
icons, the menu-bar glyph and the "no cover art" image in
modules/gui/macosx/Resources.
"""
import io, os, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', 'modules', 'gui', 'macosx', 'Resources')
ART = os.path.join(HERE, 'art')

TOP_Y, BOT_Y, TOP_HALF, BOT_HALF, CX = 190.0, 838.0, 252.0, 296.0, 512.0


def half(y):
    return TOP_HALF + (y - TOP_Y) * (BOT_HALF - TOP_HALF) / (BOT_Y - TOP_Y)


def sag(y):  # how much the "horizontal" lines bow down, for a round look
    return half(y) * 0.2


def band(y1, y2):
    a, b = half(y1), half(y2)
    return ('M%.1f,%.1f A%.1f,%.1f 0 0 0 %.1f,%.1f L%.1f,%.1f A%.1f,%.1f 0 0 1 %.1f,%.1f Z'
            % (CX - a, y1, a, sag(y1), CX + a, y1, CX + b, y2, b, sag(y2), CX - b, y2))


def barrel_svg(mono=False):
    a, b = TOP_HALF, BOT_HALF
    body = ('M%.1f,%.1f L%.1f,%.1f A%.1f,%.1f 0 0 0 %.1f,%.1f L%.1f,%.1f A%.1f,%.1f 0 0 0 %.1f,%.1f Z'
            % (CX - a, TOP_Y, CX - b, BOT_Y, b, sag(BOT_Y), CX + b, BOT_Y, CX + a, TOP_Y, a, sag(TOP_Y), CX - a, TOP_Y))
    base = ('M%.1f,838 L%.1f,900 A372,74 0 0 0 %.1f,900 L%.1f,838 A372,70 0 0 0 %.1f,838 Z'
            % (CX - 372, CX - 372, CX + 372, CX + 372, CX - 372))
    if mono:   # menu-bar template glyph: flat black shapes, bands knocked out
        return '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
<path d="%s" fill="#000"/><ellipse cx="512" cy="190" rx="252" ry="50" fill="#000"/>
<path d="%s" fill="#fff"/><path d="%s" fill="#fff"/><path d="%s" fill="#000"/></svg>''' % (
            body, band(330, 440), band(580, 690), base)
    return '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
<defs>
 <linearGradient id="sh" x1="0" x2="1" y1="0" y2="0">
  <stop offset="0" stop-color="#000" stop-opacity=".30"/><stop offset=".22" stop-color="#fff" stop-opacity=".16"/>
  <stop offset=".42" stop-color="#fff" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".38"/>
 </linearGradient>
 <linearGradient id="rub" x1="0" x2="1"><stop offset="0" stop-color="#0c0c0c"/><stop offset=".3" stop-color="#3a3a3a"/><stop offset="1" stop-color="#050505"/></linearGradient>
 <radialGradient id="gnd" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#000" stop-opacity=".45"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>
 <clipPath id="body"><path d="%(body)s"/></clipPath>
</defs>
<ellipse cx="512" cy="930" rx="430" ry="62" fill="url(#gnd)"/>
<path d="%(base)s" fill="url(#rub)"/>
<ellipse cx="512" cy="838" rx="372" ry="70" fill="#2b2b2b"/>
<ellipse cx="512" cy="838" rx="300" ry="60" fill="#111"/>
<path d="%(body)s" fill="#ff6b00"/>
<g clip-path="url(#body)">
 <path d="%(b1)s" fill="#f6f5ef"/><path d="%(b2)s" fill="#f6f5ef"/>
 <path d="%(r1)s" fill="#c94f00" opacity=".55"/><path d="%(r2)s" fill="#c94f00" opacity=".55"/><path d="%(r3)s" fill="#c94f00" opacity=".55"/>
 <rect x="150" y="100" width="724" height="860" fill="url(#sh)"/>
</g>
<ellipse cx="512" cy="190" rx="252" ry="50" fill="#ff8a2b"/>
<ellipse cx="512" cy="190" rx="212" ry="38" fill="#c94f00"/>
<path d="M430,186 C430,96 594,96 594,186" fill="none" stroke="#b34600" stroke-width="30" stroke-linecap="round"/>
<path d="M430,186 C430,104 594,104 594,186" fill="none" stroke="#ff7a14" stroke-width="14" stroke-linecap="round"/>
</svg>''' % dict(body=body, base=base, b1=band(330, 440), b2=band(580, 690),
                 r1=band(262, 272), r2=band(505, 515), r3=band(756, 766))


def render(svg, size, fmt='png'):
    return subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), '-f', fmt],
                          input=svg.encode(), capture_output=True, check=True).stdout


def icns(svg, path):
    big = Image.open(io.BytesIO(render(svg, 1024))).convert('RGBA')
    extra = [Image.open(io.BytesIO(render(svg, s))).convert('RGBA') for s in (16, 32, 64, 128, 256, 512)]
    big.save(path, format='ICNS', append_images=extra)


def doc_svg(label, barrel):
    inner = barrel[barrel.index('>') + 1:barrel.rindex('</svg>')]
    text = ('<rect x="212" y="742" width="600" height="150" rx="26" fill="#17191c"/>'
            '<text x="512" y="852" font-family="DejaVu Sans, Inter, sans-serif" font-weight="bold" font-size="%d" '
            'fill="#ff8a2b" text-anchor="middle">%s</text>' % (118 if len(label) <= 4 else 84, label)) if label else ''
    return '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
<path d="M190,40 H640 L834,234 V944 a40,40 0 0 1 -40,40 H230 a40,40 0 0 1 -40,-40 Z" fill="#f4f4f0" stroke="#b9bcc2" stroke-width="10"/>
<path d="M640,40 V194 a40,40 0 0 0 40,40 H834 Z" fill="#d5d7db" stroke="#b9bcc2" stroke-width="10" stroke-linejoin="round"/>
<g transform="translate(262,%d) scale(.488)">%s</g>%s</svg>''' % (250 if label else 300, inner, text)


def main():
    os.makedirs(ART, exist_ok=True)
    svg = barrel_svg()
    open(os.path.join(ART, 'barrel.svg'), 'w').write(svg)
    open(os.path.join(ART, 'barrel-512.png'), 'wb').write(render(svg, 512))
    icns(svg, os.path.join(RES, 'App-Icons', 'VLC.icns'))
    icns(svg, os.path.join(RES, 'App-Icons', 'VLC-Xmas.icns'))   # no seasonal cone either
    open(os.path.join(RES, 'noart.png'), 'wb').write(render(svg, 128))
    open(os.path.join(RES, 'Button-Icons', 'VLCStatusBarIcon.pdf'), 'wb').write(
        subprocess.run(['rsvg-convert', '-w', '18', '-h', '18', '-f', 'pdf'], input=barrel_svg(True).encode(),
                       capture_output=True, check=True).stdout)
    n = 0
    for f in sorted(os.listdir(os.path.join(RES, 'File-Icons'))):
        if not f.endswith('.icns'):
            continue
        name = f[:-5]
        label = '' if name == 'generic' else name.upper()
        icns(doc_svg(label, svg), os.path.join(RES, 'File-Icons', f))
        if name in ('mkv', 'generic', 'playlist'):
            open(os.path.join(ART, 'doc-%s.png' % name), 'wb').write(render(doc_svg(label, svg), 256))
        n += 1
    print('wrote app icons, menu-bar glyph, no-art image and %d document icons' % n)


main()
