#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every image store in the texture upload compute shaders is bounds-checked.

A block at the edge of an image whose size is not a multiple of the block size, and workgroups that
the dispatch rounds up past the image, store texels outside it. Some drivers drop those stores; on
AMD they land in other images' memory and can fault the GPU. The shaders therefore compare the
texel with the image's size before every `imageStore`. Source checks only (no shader is compiled):
python -I tools/ci/tests/test_host_shader_bounds.py
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SHADERS = os.path.join(ROOT, "src", "video_core", "host_shaders")

LINE_COMMENT = re.compile(r"//[^\n]*")
FUNCTION = re.compile(r"^void\s+(\w+)\s*\(", re.MULTILINE)


def source(name):
    """The shader text with line comments removed and line endings made plain."""
    with open(os.path.join(SHADERS, name), encoding="utf-8", newline="") as handle:
        return LINE_COMMENT.sub("", handle.read().replace("\r\n", "\n"))


def body(text, function):
    """The text of one top-level function, from its signature to the next top-level function."""
    found = [m for m in FUNCTION.finditer(text)]
    for i, match in enumerate(found):
        if match.group(1) == function:
            end = found[i + 1].start() if i + 1 < len(found) else len(text)
            return text[match.start():end]
    raise AssertionError(f"no function {function}")


class UnswizzleShaderTests(unittest.TestCase):
    """The block-linear and pitch unswizzle shaders return before storing outside the image."""

    def check(self, name, image):
        main = body(source(name), "main")
        stores = [m.start() for m in re.finditer(r"\bimageStore\s*\(", main)]
        self.assertEqual(len(stores), 1, name)
        guard = re.search(
            r"if\s*\(\s*any\s*\(\s*greaterThanEqual\s*\(\s*coord\s*,\s*imageSize\s*\(\s*"
            + image + r"\s*\)\s*\)\s*\)\s*\)\s*\{\s*return\s*;\s*\}", main)
        self.assertIsNotNone(guard, f"{name}: no bounds check before the store")
        self.assertLess(guard.end(), stores[0], f"{name}: the check comes after the store")

    def test_block_linear_unswizzle_2d(self):
        self.check("block_linear_unswizzle_2d.comp", "output_image")

    def test_block_linear_unswizzle_3d(self):
        self.check("block_linear_unswizzle_3d.comp", "output_image")

    def test_pitch_unswizzle(self):
        self.check("pitch_unswizzle.comp", "output_image")


class AstcDecoderTests(unittest.TestCase):
    """Every ASTC texel goes through StoreTexel, which drops texels outside the image."""

    def test_only_store_texel_stores(self):
        text = source("astc_decoder.comp")
        self.assertEqual(len(re.findall(r"\bimageStore\s*\(", text)), 1)
        self.assertIn("imageStore", body(text, "StoreTexel"))
        self.assertRegex(body(text, "StoreTexel"),
                         r"if\s*\(\s*all\s*\(\s*lessThan\s*\(\s*texel\.xy\s*,\s*image_extent\.xy\s*\)\s*\)\s*\)")

    def test_extent_is_set_before_any_decode(self):
        main = body(source("astc_decoder.comp"), "main")
        extent = main.find("image_extent = imageSize(dest_image)")
        self.assertNotEqual(extent, -1)
        self.assertLess(extent, main.find("DecompressBlock(coord)"))


if __name__ == "__main__":
    unittest.main()
