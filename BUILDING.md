# Building

Four artifacts come out of this tree:

| artifact | file |
| --- | --- |
| Linux standalone | `build/linux/circular_drum_machine` |
| Linux VST3 | `build/linux/VST3/Release/circular_drum_machine_vst3.vst3` |
| Windows standalone | `build/windows/circular_drum_machine.exe` (CI) |
| Windows VST3 | `build/windows/VST3/circular_drum_machine_vst3.vst3` (CI, one file) |

The plugin contains the full interface: its editor draws the same 880×500
canvas the app draws, into the window the host hands over (an X11 window on
Linux, a child `HWND` on Windows), and forwards mouse and keyboard to the same
`ui_t`. There is no second, smaller GUI.

## Dependencies

* cmake ≥ 3.16, a C11/C++17 compiler
* freetype2 (both targets), pkg-config
* standalone only: SDL2, GLEW, libpng, OpenGL
* plugin only: the Steinberg VST3 SDK, fetched by `scripts/fetch-vst3sdk.sh`

`vendor/sdl2/sdklocal` is used when there is no system SDL2 dev package; see the
comment in `CMakeLists.txt`.

## Linux

```sh
./scripts/build-linux.sh            # standalone and plugin
./scripts/build-linux.sh standalone
./scripts/build-linux.sh vst3       # fetches the SDK on first run
```

The result is in `build/linux`. To let hosts find the plugin, copy or link the
bundle into `~/.vst3`:

```sh
ln -s "$PWD/build/linux/VST3/Release/circular_drum_machine_vst3.vst3" ~/.vst3/
```

Plain cmake works too:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
# -DCDM_BUILD_STANDALONE=OFF / -DCDM_BUILD_VST3=OFF build one of them
```

## Windows

The Windows artifacts are built on a GitHub Actions runner
(`.github/workflows/build.yml`): msys2 with mingw-w64, its SDL2, freetype,
libpng and GLEW packages, then `./scripts/build-windows.sh all`. Pushing the
repository and downloading the `windows-standalone` / `windows-vst3` artifacts
is the intended path.

The Windows plug-in is a single `.vst3` file rather than a bundle, with the C++
runtime and freetype linked statically, so it can be dropped into the host's
VST3 folder on its own. Two SDK features are switched off there for mingw: the
validator, which the SDK builds as a GUI-subsystem binary with a `main()` and so
never links, and the bundle layout, whose post-build step probes the module
before ninja has copied it into `Contents/x86_64-win`. Linux keeps both, so the
validator still runs there.

Local cross builds from Linux work too, once mingw-w64 is installed
(`apt install g++-mingw-w64-x86-64`):

```sh
./scripts/build-windows.sh --cross vst3    # plugin: no SDL needed
CDM_WINDOWS_DEPS=/path/to/mingw/prefix \
  ./scripts/build-windows.sh --cross all   # plus the standalone
```

`CDM_WINDOWS_DEPS` must hold mingw builds of SDL2, freetype, libpng and GLEW
(`include/` and `lib/pkgconfig/`); without it the standalone target is skipped,
because a native Linux SDL2 cannot be linked into a Windows binary.

## The VST3 SDK

`scripts/fetch-vst3sdk.sh` shallow-clones the SDK into `vendor/vst3sdk` (about
250 MB with submodules) and is a no-op once it is there. It also patches one SDK
header for mingw, whose C library has `_aligned_malloc` but no C11
`std::aligned_alloc`; the patch is a no-op on the other platforms. Configure with
`-DCDM_VST3SDK_DIR=<path>` to use a copy elsewhere. The build sets
`SMTG_ENABLE_VSTGUI_SUPPORT` and the SDK's example options to `OFF`: this
project brings its own renderer, and the SDK's sample hosts want GTK.
