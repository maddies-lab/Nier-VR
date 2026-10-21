# NieR-VR

VR mod for NieR:Automata (Steam, D3D11, OpenXR). Developed and tested under Proton
on Linux with a Quest 2 over WiVRn.

## Status

- **M0** proxy DLL (`winmm.dll`), camera lookup, debug overlay
- **M1** theater mode: the game on a virtual screen
- **M2** head-tracked camera (pitch-locked game yaw + head pose)
- **M3** stereo: both eyes every frame by running the game's draw job twice
  (alternate-eye and mono modes too). Known issue: some effects differ per eye,
  see [docs/known-issues.md](docs/known-issues.md)
- **M4** HUD on its own panel; menus, maps, pause and game over on the theater screen

## Keys

| Key | Action |
| --- | --- |
| F7 | head tracking on/off (theater otherwise) |
| F5 | render mode: both eyes / mono / alternate eyes |
| F8 | recenter view, screen and HUD panel |
| Home | HUD panel on/off |
| Numpad + / - | HUD panel bigger / smaller |
| Numpad 8 / 2 | HUD panel nearer / farther |
| Numpad 4 / 6 | HUD panel higher / lower |
| Insert | debug overlay |
| F6 | pose pairing (debug) |
| F2, F3, F4, F9, F10, F11, End, PgDn/PgUp | reverse-engineering tools |

HUD settings are saved in `NieR-VR.ini` next to the game. The log is `NieR-VR.log`.

## Build (Linux, MinGW-w64)

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
tools/install.sh            # copies winmm.dll and openxr_loader.dll into the game folder
```

Steam launch options (Proton):
`PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 WINEDLLOVERRIDES="winmm=n,b" %command%`

Start the OpenXR runtime (WiVRn, SteamVR, ...) before the game. Use borderless or
windowed mode.

## Proxy exports

`src/proxy/generated` is produced by `tools/gen_proxy.py` from a reference `winmm.dll`.
It was generated from Proton's (Wine's) winmm. Regenerate from a Windows 10/11
`System32\winmm.dll` before shipping a Windows build.

## Credits

Camera, entity and player layouts from [praydog/AutomataMP](https://github.com/praydog/AutomataMP) (MIT).
Renderer facts (UIParam, tonemap vignette, SceneBuffer layout) from the development notes of
[aracielVR/NieR-Automata-in-VR](https://github.com/aracielVR/NieR-Automata-in-VR) (no code used).
Uses [MinHook](https://github.com/TsudaKageyu/minhook), [Dear ImGui](https://github.com/ocornut/imgui)
and the Khronos [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK-Source).
