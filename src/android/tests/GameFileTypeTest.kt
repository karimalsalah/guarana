// SPDX-License-Identifier: GPL-3.0-or-later

package io.github.zortos293.lichii.model

fun main() {
    val cases = mapOf(
        "Example.nup" to GameFileType.CONTAINER,
        "Example (update 2.1).NUP" to GameFileType.CONTAINER,
        "base.NsP" to GameFileType.CONTAINER,
        "cart.xci" to GameFileType.CONTAINER,
        "packed.nsz" to GameFileType.CONTAINER,
        "packed.xcz" to GameFileType.CONTAINER,
        "application.nca" to GameFileType.STANDALONE,
        "homebrew.nro" to GameFileType.STANDALONE,
        "archive.nup.zip" to GameFileType.UNSUPPORTED,
        "nup" to GameFileType.UNSUPPORTED,
        "unfinished." to GameFileType.UNSUPPORTED
    )
    for ((displayName, expected) in cases) {
        val actual = GameFileType.fromFilename(displayName)
        check(actual == expected) { "$displayName: expected $expected, got $actual" }
    }
}
