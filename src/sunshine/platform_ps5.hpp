// SPDX-License-Identifier: GPL-3.0-only
// What the PS5 platform (platform_ps5.cpp) offers the host's own code, beside Sunshine's platf interface.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platf {

// A paired device's profiles changed, by its certificate: each of its controllers sitting in a profile the device no
// longer has leaves it and only watches until a press joins it as one it still has. The stream goes on. `added` are the
// profiles it got beside those it had (none when it could choose freely before), passed only while the device streams:
// it is told at once.
void profiles_changed(const std::string &cert, const std::vector<uint32_t> &added);

}  // namespace platf
