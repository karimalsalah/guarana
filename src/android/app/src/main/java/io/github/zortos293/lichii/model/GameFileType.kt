// SPDX-License-Identifier: GPL-3.0-or-later

package io.github.zortos293.lichii.model

internal enum class GameFileType {
    CONTAINER,
    STANDALONE,
    UNSUPPORTED;

    companion object {
        fun fromFilename(displayName: String): GameFileType =
            when (displayName.substringAfterLast('.', "").lowercase()) {
                "nsp", "xci", "nsz", "xcz", "nup" -> CONTAINER
                "nca", "nro" -> STANDALONE
                else -> UNSUPPORTED
            }
    }
}
