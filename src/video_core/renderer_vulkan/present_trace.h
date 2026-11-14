// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Common {
template <std::size_t Capacity>
class EventRing;
}

namespace Vulkan {
using PresentTrace = Common::EventRing<256>;
using PresentDispatchTrace = Common::EventRing<64>;
} // namespace Vulkan
