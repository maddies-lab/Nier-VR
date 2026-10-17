#pragma once

#include <string>

namespace game {
// Writes the in-memory (SteamStub-decrypted) executable as a PE file whose sections
// are laid out at their virtual addresses, for static analysis in Ghidra/IDA.
bool dump_executable(const std::wstring& path);
} // namespace game
