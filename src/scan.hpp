#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <windows.h>

namespace scan {
struct Range {
    uintptr_t start{};
    size_t size{};

    uintptr_t end() const { return start + size; }
    bool contains(uintptr_t p) const { return p >= start && p < end(); }
};

std::optional<Range> section(HMODULE module, std::string_view name);

// First occurrence of `bytes` inside `range`.
std::optional<uintptr_t> find_bytes(Range range, std::string_view bytes);

// First occurrence of a null-terminated string inside `range`.
std::optional<uintptr_t> find_string(Range range, std::string_view str);

// First rel32 displacement inside `range` that resolves (disp + 4) to `target`.
// This is how x64 code references data: `lea reg, [rip+disp]`, `mov reg, [rip+disp]`, ...
std::optional<uintptr_t> find_rip_reference(Range range, uintptr_t target);

// IDA-style pattern ("48 8D 0D ?? ?? ?? ??") inside [start, start + length).
std::optional<uintptr_t> find_pattern(uintptr_t start, size_t length, std::string_view pattern);

// Absolute target of the rel32 displacement located at `disp_address`.
uintptr_t resolve_rel32(uintptr_t disp_address);

// MSVC RTTI: vftable of the class whose mangled type name is `mangled`
// (e.g. ".?AVcCameraGame@@"), for the complete object (offset 0).
std::optional<uintptr_t> find_vtable(HMODULE module, std::string_view mangled);
} // namespace scan
