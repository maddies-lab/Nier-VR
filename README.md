# NieR-VR

VR mod for NieR:Automata (Steam, D3D11). See the design spec for the full plan.

Current milestone: **M1 - theater mode**. The DLL loads as a `winmm.dll`
proxy, waits for SteamStub to decrypt the game, finds `cCameraGame` (validated against
its RTTI vftable) and draws a debug overlay with the live camera (Insert toggles it).
With an OpenXR runtime running, the game is shown on a virtual screen in the headset
(F8 recenters it). The headset runs on its own D3D11 device and thread; frames cross
over through shared textures and GPU-side shared fences (see `src/xr/frame_transport.hpp`).

Set `NIERVR_DUMP=1` to write the SteamStub-decrypted executable next to the game for
static analysis (Ghidra/IDA). Do not distribute it.

## Build (Linux, MinGW-w64)

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
tools/install.sh            # copies build/winmm.dll into the game folder
```

Steam launch options (Proton):
`PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 WINEDLLOVERRIDES="winmm=n,b" %command%`

Start the OpenXR runtime (e.g. WiVRn) and connect the headset before launching:
Proton sets up its OpenXR bridge only at launch.

The log is written to `NieR-VR.log` next to `NieRAutomata.exe`.

## Proxy exports

`src/proxy/generated` is produced by `tools/gen_proxy.py` from a reference `winmm.dll`.
It was generated from Proton's (Wine's) winmm. Regenerate from a Windows 10/11
`System32\winmm.dll` before shipping a Windows build.

## Credits

Camera layout and lookup method from [praydog/AutomataMP](https://github.com/praydog/AutomataMP) (MIT).
Uses [MinHook](https://github.com/TsudaKageyu/minhook), [Dear ImGui](https://github.com/ocornut/imgui)
and the [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK-Source) (shipped as `openxr_loader.dll`).
