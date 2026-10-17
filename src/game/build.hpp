#pragma once

#include <cstdint>

namespace game {
struct BuildInfo {
    uint32_t timestamp{};    // PE TimeDateStamp
    uint32_t size_of_image{};
    const char* name{};      // nullptr when the build is not in the known list
};

// Identifies the running NieRAutomata.exe from its (unencrypted) PE header.
BuildInfo identify_build();
} // namespace game
