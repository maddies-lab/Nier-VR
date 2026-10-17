#include "scan.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace scan {
std::optional<Range> section(HMODULE module, std::string_view name) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const auto len = strnlen(reinterpret_cast<const char*>(sec->Name), IMAGE_SIZEOF_SHORT_NAME);
        if (std::string_view{reinterpret_cast<const char*>(sec->Name), len} == name) {
            return Range{base + sec->VirtualAddress, sec->Misc.VirtualSize};
        }
    }

    return std::nullopt;
}

std::optional<uintptr_t> find_bytes(Range range, std::string_view bytes) {
    if (bytes.empty() || bytes.size() > range.size) {
        return std::nullopt;
    }

    const auto begin = reinterpret_cast<const char*>(range.start);
    const auto last = range.size - bytes.size();
    for (size_t i = 0; i <= last; ++i) {
        if (begin[i] == bytes[0] && std::memcmp(begin + i, bytes.data(), bytes.size()) == 0) {
            return range.start + i;
        }
    }

    return std::nullopt;
}

std::optional<uintptr_t> find_string(Range range, std::string_view str) {
    // Include the terminator so "Step" does not match "Steps".
    return find_bytes(range, std::string_view{str.data(), str.size() + 1});
}

std::optional<uintptr_t> find_rip_reference(Range range, uintptr_t target) {
    if (range.size < 4) {
        return std::nullopt;
    }

    for (uintptr_t p = range.start; p <= range.end() - 4; ++p) {
        int32_t disp{};
        std::memcpy(&disp, reinterpret_cast<const void*>(p), sizeof(disp));
        if (p + 4 + disp == target) {
            return p;
        }
    }

    return std::nullopt;
}

std::optional<uintptr_t> find_pattern(uintptr_t start, size_t length, std::string_view pattern) {
    std::vector<int> bytes;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') {
            ++i;
        } else if (pattern[i] == '?') {
            bytes.push_back(-1);
            i += (i + 1 < pattern.size() && pattern[i + 1] == '?') ? 2 : 1;
        } else {
            bytes.push_back(std::stoi(std::string{pattern.substr(i, 2)}, nullptr, 16));
            i += 2;
        }
    }

    if (bytes.size() > length) {
        return std::nullopt;
    }

    const auto mem = reinterpret_cast<const uint8_t*>(start);
    for (size_t i = 0; i <= length - bytes.size(); ++i) {
        size_t j = 0;
        while (j < bytes.size() && (bytes[j] == -1 || mem[i + j] == bytes[j])) {
            ++j;
        }
        if (j == bytes.size()) {
            return start + i;
        }
    }

    return std::nullopt;
}

uintptr_t resolve_rel32(uintptr_t disp_address) {
    int32_t disp{};
    std::memcpy(&disp, reinterpret_cast<const void*>(disp_address), sizeof(disp));
    return disp_address + 4 + disp;
}

std::optional<uintptr_t> find_vtable(HMODULE module, std::string_view mangled) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto data = section(module, ".data");
    const auto rdata = section(module, ".rdata");
    if (!data || !rdata) {
        return std::nullopt;
    }

    // TypeDescriptor { void* vftable; void* spare; char name[]; } lives in .data.
    const auto name = find_string(*data, mangled);
    if (!name) {
        return std::nullopt;
    }
    const auto type_descriptor_rva = static_cast<uint32_t>(*name - 0x10 - base);

    // CompleteObjectLocator (x64) lives in .rdata:
    // { u32 signature = 1; u32 offset; u32 cd_offset; u32 type_rva; u32 hierarchy_rva; u32 self_rva; }
    const auto words = reinterpret_cast<const uint32_t*>(rdata->start);
    const auto count = rdata->size / sizeof(uint32_t);
    for (size_t i = 0; i + 6 <= count; ++i) {
        const auto col = rdata->start + i * sizeof(uint32_t);
        if (words[i] != 1 || words[i + 1] != 0 || words[i + 3] != type_descriptor_rva ||
            words[i + 5] != static_cast<uint32_t>(col - base)) {
            continue;
        }

        // vftable[-1] points at the locator.
        const auto col_ref = find_bytes(*rdata, std::string_view{reinterpret_cast<const char*>(&col), sizeof(col)});
        if (col_ref) {
            return *col_ref + sizeof(uintptr_t);
        }
    }

    return std::nullopt;
}
} // namespace scan
