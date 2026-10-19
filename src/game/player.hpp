#pragma once

#include <cstdint>
#include <optional>

// The entity the player controls (2B / 9S / A2 / flight unit), via the game's
// entity list. Layouts and lookups from praydog/AutomataMP (MIT).
namespace game::player {
// Finds the entity list and the possessed-entity handle. Call once after the
// executable is decrypted.
bool locate();

// Health of the possessed entity, if it can be read.
std::optional<uint32_t> health();
} // namespace game::player
