#!/usr/bin/env python3
"""Derives the Guarana Dark theme (dist/qt_themes/guarana) from Lichii Dark (dist/qt_themes/lichii).

Run from the repository root after changing the Lichii theme or the palette below:
    python tools/ui/make_guarana_theme.py
The icon recolouring needs Pillow (pip install pillow); the stylesheet and the resource list do not.
The generated files are committed, so building needs neither.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "dist" / "qt_themes"
SRC = ROOT / "lichii"
DST = ROOT / "guarana"

# Lichii's neutral greys and pink accent -> the launcher's navy surfaces and the ember accent.
COLORS = {
    "#17181b": "#0b101d", "#131416": "#080c16", "#141518": "#0a0f1b", "#1b1c20": "#0f1627",
    "#202125": "#0e1527", "#232429": "#141d33", "#2a2c31": "#1b2640", "#303238": "#222f4d",
    "#34363c": "#2a3652", "#2a2b30": "#1f2a44", "#212226": "#182036", "#44464d": "#3a4868",
    "#ececef": "#eef1f6", "#c4c5ca": "#c2c9d6", "#9b9da4": "#8f99ad", "#6f7178": "#6b7589",
    "#55575d": "#4d576b", "#d6d7db": "#d3d9e4", "#dcdde1": "#dde2ec", "#8b8d94": "#8f99ad",
    "#4a4c53": "#33405d", "#1f2024": "#0f1627", "#1d1e22": "#0f1627", "#2f3137": "#1b2640",
    "#f0386b": "#f2851a", "#f45a85": "#f99a3c", "#d42c5a": "#d9720f",
    "#3a1d28": "#33210f", "#5a2f3d": "#5c3a14", "#3a2230": "#33210f", "#2c1a21": "#2a1b0c",
    "rgba(240, 56, 107, 0.16)": "rgba(242, 133, 26, 0.16)",
    "rgba(240,56,107,0.16)": "rgba(242,133,26,0.16)",
    "rgba(240, 56, 107, 0.55)": "rgba(251, 191, 36, 0.70)",
    "rgba(240, 56, 107, 0.45)": "rgba(242, 133, 26, 0.45)",
    "rgba(240, 56, 107, 0.35)": "rgba(242, 133, 26, 0.35)",
}
EXTRA = """
/* Guarana additions ----------------------------------------------------- */

/* Text on an amber fill is dark ink, as on the launcher's primary button. */
QPushButton:default,
QPushButton:default:hover,
QPushButton:default:pressed {
    color: #1a1204;
}

/* Focus ring for keyboard and controller navigation: 2 px amber on anything that takes focus. */
QPushButton:focus {
    border: 2px solid #fbbf24;
    padding: 0 15px;
}

QToolButton:focus,
QTabBar::tab:focus {
    border-color: #fbbf24;
}

QLineEdit:focus,
QAbstractSpinBox:focus,
QComboBox:focus,
QComboBox:on,
QPlainTextEdit:focus,
QTextEdit:focus {
    border-color: #fbbf24;
}

QListView::item:selected,
QTreeView::item:selected,
QTableView::item:selected {
    background-color: rgba(242, 133, 26, 0.22);
    color: #ffffff;
}

QListView:focus,
QTreeView:focus,
QTableView:focus {
    border: 2px solid rgba(251, 191, 36, 0.70);
}

QListView::item:focus,
QTreeView::item:focus {
    outline: 2px solid #fbbf24;
    outline-offset: -2px;
}

QToolTip {
    border-radius: 8px;
}

/* First-run panel and the in-game toast (src/lichii/guarana_ui.cpp). */
QFrame#GuaranaEmptyPanel {
    background-color: #0e1527;
    border: 1px solid #1f2a44;
    border-radius: 8px;
}

QFrame#GuaranaCard {
    background-color: #141d33;
    border: 1px solid #1f2a44;
    border-radius: 8px;
}

QFrame#GuaranaCard[active="true"] {
    border: 2px solid #f2851a;
}

QLabel#GuaranaHeading {
    font-size: 20px;
    font-weight: 600;
    color: #eef1f6;
}

QLabel#GuaranaHelp {
    color: #8f99ad;
}

QLabel#GuaranaToast {
    background-color: rgba(11, 16, 29, 0.92);
    color: #eef1f6;
    border: 1px solid #2a3652;
    border-radius: 8px;
    padding: 8px 14px;
}
"""


def stylesheet():
    text = (SRC / "style.qss").read_text(encoding="utf-8")
    for old, new in COLORS.items():
        text = re.sub(re.escape(old), new, text, flags=re.I)
    text = re.sub(r"border-radius:\s*(5|6|7|10)px", "border-radius: 8px", text)
    text = text.replace(":/lichii/icons/", ":/guarana/icons/")
    text = text.replace("Lichii Dark. Tokens", "Guarana Dark, derived from Lichii Dark by tools/ui/make_guarana_theme.py. Tokens")
    (DST / "style.qss").write_text(text + EXTRA, encoding="utf-8")


def recolor():
    from PIL import Image
    amber, ink = (0xF2, 0x85, 0x1A), (0x1A, 0x12, 0x04)
    out = DST / "icons"
    out.mkdir(parents=True, exist_ok=True)
    changed = set()
    for path in sorted((SRC / "icons").glob("*.png")):
        im = Image.open(path).convert("RGBA")
        px = im.load()
        w, h = im.size
        if not any(px[x, y][3] > 20 and px[x, y][0] > 180 and px[x, y][0] - px[x, y][1] > 100 for x in range(w) for y in range(h)):
            continue
        white_to = ink if path.name.startswith("check_") else (255, 255, 255)
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                if a == 0:
                    continue
                t = min(1.0, max(0.0, (g - 56) / 199))
                px[x, y] = tuple(round(amber[i] * (1 - t) + white_to[i] * t) for i in range(3)) + (a,)
        im.save(out / path.name)
        changed.add(path.name)
    return changed


def resources(changed):
    names = sorted(p.name for p in (SRC / "icons").glob("*.png"))
    lines = ['<!--\nSPDX-FileCopyrightText: Copyright 2026 Guarana contributors\nSPDX-License-Identifier: GPL-3.0-or-later\n-->\n',
             "<!-- Generated by tools/ui/make_guarana_theme.py. Icons without the pink accent are shared with Lichii Dark. -->",
             "<RCC>", '    <qresource prefix="icons/guarana">',
             '        <file alias="index.theme">index.theme</file>', "    </qresource>",
             '    <qresource prefix="guarana">', "        <file>style.qss</file>"]
    for n in names:
        src = f"icons/{n}" if n in changed else f"../lichii/icons/{n}"
        lines.append(f'        <file alias="icons/{n}">{src}</file>')
    lines += ["    </qresource>", "</RCC>", ""]
    (DST / "style.qrc").write_text("\n".join(lines), encoding="utf-8")
    (DST / "index.theme").write_text("[Icon Theme]\nName=guarana\nComment=Guarana Dark\nInherits=colorful_dark\nDirectories=16x16\n\n[16x16]\nSize=16\n", encoding="utf-8")


if __name__ == "__main__":
    DST.mkdir(exist_ok=True)
    stylesheet()
    try:
        changed = recolor()
    except ImportError:
        sys.exit("Pillow is needed to recolour the icons: pip install pillow")
    resources(changed)
    print(f"{len(changed)} icons recoloured")
