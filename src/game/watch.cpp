#include "game/watch.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "game/camera.hpp"
#include "log.hpp"

namespace game::watch {
namespace {
constexpr size_t k_max_hits = 512;

struct Hit {
    std::atomic<uintptr_t> address{0}; // instruction after the access (data breakpoints trap afterwards)
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> slots{0};    // DR6 bits 0-3; bit 4 = DR6 not reported
};

Hit g_hits[k_max_hits];
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_unrecorded{0};
Target g_targets[4]{};

// Runs on game threads inside the exception dispatcher: no locks, no allocation.
void record(uintptr_t address, uint32_t slots) {
    const size_t start = (address >> 2) % k_max_hits;
    for (size_t i = 0; i < k_max_hits; ++i) {
        auto& hit = g_hits[(start + i) % k_max_hits];
        uintptr_t current = hit.address.load();
        if (current == 0 && hit.address.compare_exchange_strong(current, address)) {
            current = address;
        }
        if (current == address) {
            hit.count.fetch_add(1);
            hit.slots.fetch_or(slots);
            return;
        }
    }
    g_unrecorded.fetch_add(1);
}

LONG CALLBACK exception_handler(EXCEPTION_POINTERS* info) {
    if (!g_active || info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto slots = static_cast<uint32_t>(info->ContextRecord->Dr6 & 0xF);
    if (slots == 0) {
        slots = 0x10;
    }
    record(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress), slots);
    info->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Writes DR0-3/DR7 on every thread of the process except the calling one.
int set_debug_registers(bool arm) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    uint64_t dr7 = 0;
    if (arm) {
        for (int i = 0; i < 4; ++i) {
            if (g_targets[i].address == 0) {
                continue;
            }
            dr7 |= 1ull << (i * 2);                                                // local enable
            dr7 |= static_cast<uint64_t>(g_targets[i].access) << (16 + i * 4);     // RW
            dr7 |= 2ull << (18 + i * 4);                                           // LEN = 8 bytes
        }
    }

    const auto pid = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    int threads = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }
        const auto thread =
            OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }

        SuspendThread(thread);
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
            context.Dr0 = arm ? g_targets[0].address : 0;
            context.Dr1 = arm ? g_targets[1].address : 0;
            context.Dr2 = arm ? g_targets[2].address : 0;
            context.Dr3 = arm ? g_targets[3].address : 0;
            context.Dr6 = 0;
            context.Dr7 = dr7;
            threads += SetThreadContext(thread, &context) ? 1 : 0;
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }

    CloseHandle(snapshot);
    return threads;
}

void log_results() {
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    struct Row {
        uintptr_t address;
        uint32_t count, slots;
    };
    std::vector<Row> rows;
    for (auto& hit : g_hits) {
        if (const auto address = hit.address.load()) {
            rows.push_back({address, hit.count.load(), hit.slots.load()});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });

    logger::info("[watch] %zu distinct instructions (%llu hits not recorded); addresses are the instruction AFTER the access",
                 rows.size(), (unsigned long long)g_unrecorded.load());
    for (const auto& row : rows) {
        char labels[160]{};
        for (int i = 0; i < 4; ++i) {
            if (row.slots & (1u << i)) {
                std::snprintf(labels + strlen(labels), sizeof(labels) - strlen(labels), " [%s]", g_targets[i].label);
            }
        }
        if (row.slots & 0x10) {
            std::snprintf(labels + strlen(labels), sizeof(labels) - strlen(labels), " [DR6 not reported]");
        }
        const bool in_exe = row.address >= exe && row.address < exe + 0x2000000;
        logger::info("[watch] %s+0x%llx  x%u%s", in_exe ? "exe" : "abs",
                     (unsigned long long)(in_exe ? row.address - exe : row.address), row.count, labels);
    }
}
} // namespace

void start(const Target (&targets)[4], uint32_t duration_ms) {
    if (g_active.exchange(true)) {
        return;
    }

    for (auto& hit : g_hits) {
        hit.address = 0;
        hit.count = 0;
        hit.slots = 0;
    }
    g_unrecorded = 0;
    std::copy(std::begin(targets), std::end(targets), g_targets);

    std::thread{[duration_ms] {
        static PVOID handler = AddVectoredExceptionHandler(1, exception_handler);
        (void)handler;

        for (const auto& t : g_targets) {
            if (t.address != 0) {
                logger::info("[watch] %s at %p (%s)", t.label, (void*)t.address,
                             t.access == Access::write ? "write" : "read/write");
            }
        }
        const auto armed = set_debug_registers(true);
        logger::info("[watch] armed on %d threads for %u ms", armed, duration_ms);
        Sleep(duration_ms);
        set_debug_registers(false);
        Sleep(50); // let in-flight handlers finish
        log_results();
        g_active = false;
    }}.detach();
}

void start_camera_watch() {
    const auto camera = CameraGame::get();
    if (camera == nullptr || !camera->valid() || camera->data() == nullptr) {
        logger::warn("[watch] no valid camera");
        return;
    }

    const auto data = reinterpret_cast<uintptr_t>(camera->data());
    const Target targets[4] = {
        {"view write (data+0x00)", data + 0x00, Access::write},
        {"projection write (data+0x40)", data + 0x40, Access::write},
        {"viewproj access (data+0x80)", data + 0x80, Access::read_write},
        {"eye write (cam+0x10)", reinterpret_cast<uintptr_t>(camera) + 0x10, Access::write},
    };
    start(targets, 3000);
}
} // namespace game::watch
