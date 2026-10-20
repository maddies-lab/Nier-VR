#pragma once

#include <d3d11.h>

// M4: the HUD on its own layer. NieR draws its UI last, through an orthographic
// SceneBuffer (1600x900 canvas), into the full-size R11G11B10F scene target. In
// head-tracked rendering those draws are redirected into a transparent
// premultiplied-alpha texture (left / only pass) or skipped (right pass), so the
// eye images are clean and the HUD is shown as a quad in front of the player.
namespace game::hud {
enum class Mode {
    off,      // draws untouched (theater screen, HUD layer disabled)
    capture,  // UI draws go to the HUD texture
    suppress, // UI draws are skipped (second eye pass)
};

// Registers the context observers and the draw interceptor (game's context).
void install(ID3D11DeviceContext* context);

// Draw-job thread, around each pass.
void begin_pass(ID3D11DeviceContext* context, Mode mode);
void end_pass();

// The HUD texture of the last capture pass (null until a UI draw was seen), and
// whether that pass drew any UI.
ID3D11Texture2D* texture();
bool captured_any();

// Home toggles the HUD layer (render thread).
void on_present();
bool enabled();
} // namespace game::hud
