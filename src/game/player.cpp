#include "game/player.hpp"

#include <cstring>

#include <windows.h>

#include "log.hpp"
#include "scan.hpp"

namespace game::player {
namespace {
// EntityList: array of { u32 handle; Entity* entity; } (16-byte stride).
// Entity +0x48: Behavior*; BehaviorAppBase +0x858: u32 health.
constexpr uintptr_t k_entity_behavior = 0x48;
constexpr uintptr_t k_behavior_health = 0x858;
constexpr uint32_t k_max_entities = 0x10000;

uintptr_t* g_entity_list = nullptr; // EntityList**
uint32_t* g_possessed_handle = nullptr;

bool readable(uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (address < 0x10000 || VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    const auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    const DWORD readable_protection = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & readable_protection) != 0 && (info.Protect & PAGE_GUARD) == 0 &&
           address + size <= end;
}

template <typename T>
std::optional<T> read(uintptr_t address) {
    if (!readable(address, sizeof(T))) {
        return std::nullopt;
    }
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}
} // namespace

bool locate() {
    const auto exe = GetModuleHandleW(nullptr);
    const auto text = scan::section(exe, ".text");
    const auto rdata = scan::section(exe, ".rdata");
    if (!text || !rdata) {
        return false;
    }

    if (const auto ref = scan::find_pattern(
            text->start, text->size, "48 C1 E1 04 48 03 ?? ?? ?? ?? ?? 8B 01 33 C2 A9 00 FF FF FF 75 0F 48 8B 41 08")) {
        g_entity_list = reinterpret_cast<uintptr_t*>(scan::resolve_rel32(*ref + 7));
    }

    // "core_AccelTime_Out" is referenced right before a lea of the handle.
    if (const auto str = scan::find_string(*rdata, "core_AccelTime_Out")) {
        if (const auto str_ref = scan::find_rip_reference(*text, *str)) {
            if (const auto lea = scan::find_pattern(*str_ref + 4, 10, "48 8D 15")) {
                g_possessed_handle = reinterpret_cast<uint32_t*>(scan::resolve_rel32(*lea + 3));
            }
        }
    }

    const auto base = reinterpret_cast<uintptr_t>(exe);
    logger::info("[player] entity list %s, possessed handle %s",
                 g_entity_list ? logger::format("exe+0x%llx", (unsigned long long)((uintptr_t)g_entity_list - base)).c_str()
                               : "not found",
                 g_possessed_handle
                     ? logger::format("exe+0x%llx", (unsigned long long)((uintptr_t)g_possessed_handle - base)).c_str()
                     : "not found");
    return g_entity_list != nullptr && g_possessed_handle != nullptr;
}

std::optional<uint32_t> health() {
    if (g_entity_list == nullptr || g_possessed_handle == nullptr) {
        return std::nullopt;
    }
    const uint32_t handle = *g_possessed_handle;
    const uint32_t index = (handle >> 8) & 0xFFFF;
    const auto list = read<uintptr_t>(reinterpret_cast<uintptr_t>(g_entity_list));
    if (!list || *list == 0 || handle == 0 || index >= k_max_entities) {
        return std::nullopt;
    }
    const uintptr_t descriptor = *list + uintptr_t(index) * 16;
    const auto stored_handle = read<uint32_t>(descriptor);
    const auto entity = read<uintptr_t>(descriptor + 8);
    if (!stored_handle || *stored_handle != handle || !entity || *entity == 0) {
        return std::nullopt;
    }
    const auto behavior = read<uintptr_t>(*entity + k_entity_behavior);
    if (!behavior || *behavior == 0) {
        return std::nullopt;
    }
    return read<uint32_t>(*behavior + k_behavior_health);
}
} // namespace game::player
