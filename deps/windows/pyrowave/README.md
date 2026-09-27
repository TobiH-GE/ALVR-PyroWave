# PyroWave for the Windows streamer

`deps/` is ignored by git because it holds built binaries; this README is the only tracked
file in it. The streamer builds without anything here. When the files below are present,
`alvr/server_openvr/build.rs` defines `ALVR_PYROWAVE` and compiles
`cpp/platform/win32/VideoEncoderPyroWave.cpp`, and `cargo xtask build-streamer` copies the
DLL next to the driver.

Only headers are needed at build time. The DLL is loaded at runtime with `LoadLibrary`, so
nothing is linked against it.

## Layout

```
deps/windows/pyrowave/
  include/
    pyrowave/pyrowave.h          from the PyroWave checkout
    vulkan/...                   Vulkan headers PyroWave was built with
    vk_video/...
  bin/
    pyrowave-shared-0.dll        the name depends on the toolchain, any *pyrowave*.dll is copied
```

## Building PyroWave

Use the same PyroWave commit for the streamer and for the visionOS client
(`alvr-visionos` vendors `metal/` from it). The bitstream is what has to match, and it is
still a draft. This project was set up against
`89f7e47d4abbf650c91fae766728af866c5e32a0` (C API 0.6.0).

Needs Visual Studio 2022, CMake 3.27 or newer, and Git Bash (for the checkout script).

```
git clone https://github.com/Themaister/pyrowave
cd pyrowave
git checkout 89f7e47d4abbf650c91fae766728af866c5e32a0
bash checkout_granite.sh
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_INSTALL_PREFIX=build/output
cmake --build build --config Release --target install
```

Then copy into this folder:

| from | to |
|---|---|
| `build/output/include/pyrowave/pyrowave.h` | `include/pyrowave/pyrowave.h` |
| `Granite/third_party/khronos/vulkan-headers/include/vulkan` | `include/vulkan` |
| `Granite/third_party/khronos/vulkan-headers/include/vk_video` | `include/vk_video` |
| `build/output/bin/pyrowave-shared-0.dll` | `bin/` |

`build/output/bin/pyrowave-device-validation.exe` is installed as well. Running it once on
the streaming PC checks that the GPU and driver have what the encoder needs.

## Runtime requirements

- A Vulkan 1.3 driver for the GPU SteamVR renders on. Current NVIDIA drivers ship one.
  PyroWave picks the Vulkan device by the D3D11 adapter's LUID, so it always runs on the
  same GPU as the game.
- Windows 10 1703 or later (D3D11.4 shared fences).
- SDR only for now: the encoder refuses to start with HDR enabled.
