# LibertyDroid7

[Español](README.md) | **English**

Unofficial native port of **Grand Theft Auto IV** and **Episodes from Liberty City** (Xbox 360) to **Android**, with its own launcher.

The game is not emulated. Its Xbox 360 executable is recompiled to native ARM64 code and drawn by a Vulkan renderer. The project also includes the launcher that imports the game, configures it and starts it.

## Contents

- [Status](#status)
- [What it includes](#what-it-includes)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Dependencies](#dependencies)
- [Building](#building)
- [Repository layout](#repository-layout)
- [Credits](#credits)
- [License](#license)
- [Legal notice](#legal-notice)

## Status

The game is playable on Android with Mali GPUs (tested on Mali-G615) and Adreno GPUs (tested on Adreno 610), with no known graphical errors. Performance depends mostly on the phone's CPU, temperature and RAM.

Still to do:

- Build from a clean clone, which has not been tried yet.
- Warn in the launcher when an imported driver is damaged. Today the game rejects it at startup.

## What it includes

### Launcher

Native C++ launcher, using ImGui on OpenGL ES 3:

- **Import ISO:** installs the game from the ISO, with no need to copy files over adb.
- **Choose game:** GTA IV or Episodes from Liberty City, from a single app.
- **Graphics:** Ultra performance, Performance, Default, Quality and Native (Xbox 360) presets, plus a Custom mode for shadows, view distance, detail, population and reflections.
- **GPU and drivers:** on Adreno, choose between automatic, the system driver, Turnip or an imported AdrenoTools driver. If the imported driver can't be used, the automatic choice applies.
- **Saves:** manual and automatic backups, and restore.
- **Controls:** configurable touch controls (opacity, size, vibration) and gamepad.
- **System:** language, FPS counter, game file check and last error.
- **Languages:** Spanish, English, French, German and Italian, for the game as well.

### Port

- Reimplemented Xbox 360 kernel: memory, threads, files, input and saves.
- Native Vulkan renderer, with FXAA and SMAA antialiasing.
- Sun shadows stabilized while driving.
- Adjustable fog and view distance, with dynamic distance.
- Vector fonts for sharp text.
- Support for widescreen phone displays, including the minimap.
- Pause and thread hold when the app goes to the background.
- Guards against crashes in the game's physics engine and task pool.
- Shaders without `shaderInt64`, for GPUs that lack it.
- A PC test window with keyboard and gamepad (`GTA4_PC_WINDOW=1`).

## How it works

1. **Static recompilation:** [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) translates the PowerPC code in `default.xex` to C++, which is then compiled for ARM64.
2. **Runtime:** `runtime/` replaces the console's system. It implements the kernel and XAM functions the game uses, and handles memory and byte order.
3. **Hooks:** `rexglue_glue/` replaces specific game functions with its own versions, identified by address.
4. **Renderer:** the hooks intercept the game's graphics commands, which a Vulkan renderer based on LibertyRecomp draws using translated Xbox 360 shaders.
5. **Two games, one codebase:** the sources are written for the addresses of Episodes from Liberty City. `tools/gen_iv_sources.py` generates the GTA IV copy at build time, using the tables in `analysis/gta4/`.
6. **Drivers on Adreno:** [libadrenotools](https://github.com/bylaws/libadrenotools) loads Turnip or an imported driver when the phone's own driver falls short.

## Requirements

### To play

- Android 10 or later, ARM64.
- A GPU with Vulkan 1.2 or later. On Adreno, Turnip can cover this if the phone's driver falls short.
- An ISO of the **USA version** of GTA IV or Episodes from Liberty City, with no title updates. Other versions don't match the recompiled code.

### To build

- Windows with Git Bash, Python 3 and Node.js.
- Android SDK (build-tools 36.0.0, platform 34, CMake 3.31.6) and NDK 27.2.12479018.
- JDK and Visual Studio Build Tools 2022.

## Dependencies

`tools/fetch-deps.sh` downloads the main ones at the exact versions used and applies the changes in `patches/`.

### Recompilation and base

| Project | Use | License |
|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | PowerPC to C++ recompiler (patched) | MIT |
| [XenosRecomp](https://github.com/sonicnext-dev/XenosRecomp) | Xbox 360 shader translation | MIT |
| [LibertyRecomp](https://github.com/vaduur/LibertyRecompAndroid) | Native renderer, hooks and resources | GPL-3.0 |
| ReXGlue | Base of the renderer and runtime (included, modified) | BSD |
| [Xenia Canary](https://github.com/xenia-canary/xenia-canary) | GPU and support libraries (patched) | BSD |

### Android and GPU

| Project | Use | License |
|---|---|---|
| [libadrenotools](https://github.com/bylaws/libadrenotools) | Loading custom drivers on Adreno | BSD-2-Clause |
| [Turnip (Mesa)](https://github.com/K11MCH1/AdrenoToolsDrivers) | Vulkan driver for Adreno, v26.0.0-rc08 | MIT |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Vulkan headers | Apache-2.0 / MIT |
| [volk](https://github.com/zeux/volk) | Vulkan loader | MIT |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU memory | MIT |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | SPIR-V processing | MIT, Apache-2.0 |
| [glslang](https://github.com/KhronosGroup/glslang) | GLSL compiler | BSD and others |
| [DXC (binaries)](https://github.com/renderbag/dxc-bin) | HLSL compiler, at build time only | NCSA and others |
| [smol-v](https://github.com/aras-p/smol-v) | SPIR-V compression | MIT / public domain |

### Libraries

| Project | Use | License |
|---|---|---|
| [o1heap](https://github.com/pavel-kirienko/o1heap) | Game heap | MIT |
| [unordered_dense](https://github.com/martinus/unordered_dense) | Hash tables | MIT |
| libmspack (included) | LZX decompression | LGPL-2.1 |
| [zstd](https://github.com/facebook/zstd) | Compression | BSD / GPL-2.0 |
| [snappy](https://github.com/google/snappy) | Compression | BSD-3-Clause |
| [FFmpeg](https://github.com/has207/FFmpeg) | XMA audio decoding | LGPL / GPL |
| [Dear ImGui](https://github.com/ocornut/imgui) | Launcher interface | MIT |
| [simde](https://github.com/simd-everywhere/simde) | Portable SIMD | MIT |
| [spdlog](https://github.com/gabime/spdlog), [fmt](https://github.com/fmtlib/fmt) | Logging and text formatting | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | TOML parsing | MIT |
| [CLI11](https://github.com/CLIUtils/CLI11) | Command line | BSD-3-Clause |
| [xxHash](https://github.com/Cyan4973/xxHash) | Hashes | BSD-2-Clause |
| [utfcpp](https://github.com/nemtrif/utfcpp) | UTF-8 text | BSL-1.0 |
| picosha2, RenderDoc API (included) | SHA-256 and graphics debugging | MIT |

## Building

> **Note:** building from a clean clone has not been tried yet. Some steps will probably need adjusting.

1. **Dependencies:** from the repository root, run `bash tools/fetch-deps.sh`. Download Turnip from [AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers) and place it in `third_party/turnip/v26.0.0-R8/`.
2. **LibertyRecomp files:** copy `LibertyRecompLib` from [LibertyRecomp](https://github.com/vaduur/LibertyRecompAndroid) into `third_party/rexglue/` and generate the shader caches. `rexglue_glue/shader_override_cache.cpp` is generated with `tools/gta4_shader_override_compiler.py`.
3. **Recompile the game:** extract `default.xex` from the ISO and place it in `private/gta4/default.xex` (GTA IV) and `private/default.xex` (Episodes from Liberty City). Build XenonRecomp with `build-xenonrecomp.bat`, then run `tools\recompile-iv.bat` and `tools\recompile.bat`.
4. **Backgrounds:** add `LAUNCHER/FONDO DE MALI.png` and `LAUNCHER/FONDO DE ADRENO.png`.
5. **APK:**

   ```bash
   bash tools/build-android.sh iv gta4app
   bash tools/build-android.sh gta4app
   bash tools/build-apk.sh
   ```

6. **Install:** run `adb install -r build/apk/gta4port.apk`, open the app and import the ISO from the launcher.

## Repository layout

| Folder | Contents |
|---|---|
| `runtime/` | Xbox 360 kernel, memory, audio, launcher (`runtime/os/launcher*`) and Android code |
| `rexglue_glue/` | Hooks on the recompiled code and game options |
| `third_party/rexglue/` | ReXGlue and the native renderer, with the port's changes |
| `third_party/libmspack/` | LZX decompression |
| `xenia_glue/` | Link to Xenia's GPU |
| `android/`, `android-snap/` | Android activity and manifests |
| `config/` | Recompilation configuration |
| `analysis/` | Address tables between game versions |
| `cmake/` | Library builds |
| `tools/` | Build, analysis and generation scripts |
| `patches/` | Changes to Xenia and XenonRecomp |

## Credits

This project exists thanks to the work of:

- **LibertyRecomp:** native renderer and game hooks.
- **Tom Clay:** ReXGlue.
- **Ben Vanik and the Xenia team:** Xenia, and Xenia Canary.
- **hedge-dev:** XenonRecomp and [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), a reference for recompiling an Xbox 360 game.
- **sonicnext-dev:** XenosRecomp.
- **Billy Laws:** libadrenotools.
- **The Mesa project and KIMCHI (K11MCH1):** Turnip and its Android builds.
- **ThirteenAG and contributors:** FusionFix, a reference for several graphics effects.
- **The authors of the libraries** listed under [Dependencies](#dependencies).
- **Rockstar Games:** Grand Theft Auto IV and Episodes from Liberty City.

## License

[GPL-3.0](LICENSE), the license LibertyRecomp's code requires. Third-party parts keep their own licenses and copyright notices.

## Legal notice

Fan project, not affiliated with or endorsed by Rockstar Games or Take-Two Interactive. Grand Theft Auto and its names and trademarks belong to their owners. No game files are distributed. Do not request or share ISOs, executables or game data in this repository.
