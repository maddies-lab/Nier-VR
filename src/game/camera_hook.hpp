#pragma once

// Head-tracked game camera (M2): detours the per-frame camera matrix update
// (exe+0x274F90 on the 2021 Steam build) and feeds it a head-rotated eye/target/up
// plus the headset's frustum, so view, view*projection and the culling frustum are
// all rebuilt by the game itself.
namespace game::camera_hook {
bool install();
} // namespace game::camera_hook
