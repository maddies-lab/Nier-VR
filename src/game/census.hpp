#pragma once

#include <d3d11.h>

// Reverse-engineering helper (M3 step 2): for one frame, records the call stack
// of every Draw/DrawIndexed/Map on the game's immediate context and logs each
// distinct call path with its count, to map which engine functions issue the
// scene, shadow and UI work. Bound to F11.
namespace game::census {
// Render thread, from Present. Arms on F11 and finishes the recording.
void on_present(ID3D11DeviceContext* context);
} // namespace game::census
