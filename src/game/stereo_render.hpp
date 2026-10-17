#pragma once

#include <d3d11.h>

// Both eyes every frame (M3 step 2). The game records each frame into a render
// queue that a draw job (exe+0x756360) executes on a worker thread; all constant
// buffers are uploaded on the main thread before it runs. The job is run twice:
// left eye (as the game uploaded it), the backbuffer goes to the headset, the main
// camera's SceneBuffer is re-uploaded with the right eye, and the job runs again.
namespace game::stereo_render {
bool install();

// Render thread, from Present: hooks the immediate context once it exists.
void on_present(ID3D11DeviceContext* context);
} // namespace game::stereo_render
