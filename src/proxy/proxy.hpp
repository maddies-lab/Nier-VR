#pragma once

#include <cstddef>

namespace proxy {
struct Export {
    const char* name; // nullptr for ordinal-only exports
    unsigned short ordinal;
};

// Defined by the generated <name>_table.cpp.
extern const wchar_t* const k_system_dll;
extern const Export k_exports[];
extern const size_t k_export_count;
extern void** const k_slots;

// Loads the real DLL from the system directory and points every stub at it.
// Exports the system DLL lacks are pointed at a stub that returns 0.
bool load_system_dll();
} // namespace proxy
