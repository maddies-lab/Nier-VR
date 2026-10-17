#include "game/dump.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

#include "log.hpp"

namespace game {
bool dump_executable(const std::wstring& path) {
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto image_size = nt->OptionalHeader.SizeOfImage;

    // Copy page by page, skipping anything not readable (left zeroed).
    std::vector<uint8_t> image(image_size);
    for (size_t offset = 0; offset < image_size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(base + offset, &info, sizeof(info)) == 0) {
            break;
        }
        const auto region_end = static_cast<size_t>(static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - base);
        const auto end = region_end < image_size ? region_end : image_size;
        const bool readable = info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
        if (readable) {
            std::memcpy(image.data() + offset, base + offset, end - offset);
        }
        offset = end;
    }

    // Raw layout = virtual layout, and the image base = where it was actually loaded,
    // so relocated pointers in .data/.rdata stay consistent.
    auto out_nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image.data() + (reinterpret_cast<const uint8_t*>(nt) - base));
    out_nt->OptionalHeader.ImageBase = reinterpret_cast<uintptr_t>(base);
    out_nt->OptionalHeader.FileAlignment = out_nt->OptionalHeader.SectionAlignment;
    auto section = IMAGE_FIRST_SECTION(out_nt);
    for (WORD i = 0; i < out_nt->FileHeader.NumberOfSections; ++i, ++section) {
        const auto align = out_nt->OptionalHeader.SectionAlignment;
        section->PointerToRawData = section->VirtualAddress;
        section->SizeOfRawData = (section->Misc.VirtualSize + align - 1) / align * align;
    }

    const auto file = _wfopen(path.c_str(), L"wb");
    if (file == nullptr) {
        logger::error("[dump] could not open output file");
        return false;
    }
    const auto written = std::fwrite(image.data(), 1, image.size(), file);
    std::fclose(file);

    logger::info("[dump] wrote %zu bytes (image base %p)", written, (const void*)base);
    return written == image.size();
}
} // namespace game
