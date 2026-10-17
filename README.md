# NieR-VR

VR mod for NieR:Automata (Steam, D3D11). See the design spec for the full plan.

Current milestone: **M0 - environment and RE baseline**. The DLL loads as a `winmm.dll`
proxy, waits for SteamStub to decrypt the game, finds `cCameraGame` (validated against
its RTTI vftable) and draws a debug overlay with the live camera (Insert toggles it).

## Build (Linux, MinGW-w64)

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
tools/install.sh            # copies build/winmm.dll into the game folder
```

Steam launch options (Proton): `WINEDLLOVERRIDES="winmm=n,b" %command%`

The log is written to `NieR-VR.log` next to `NieRAutomata.exe`.

## Proxy exports

`src/proxy/generated` is produced by `tools/gen_proxy.py` from a reference `winmm.dll`.
It was generated from Proton's (Wine's) winmm. Regenerate from a Windows 10/11
`System32\winmm.dll` before shipping a Windows build.

## Credits

Camera layout and lookup method from [praydog/AutomataMP](https://github.com/praydog/AutomataMP) (MIT).
Uses [MinHook](https://github.com/TsudaKageyu/minhook) and [Dear ImGui](https://github.com/ocornut/imgui).
