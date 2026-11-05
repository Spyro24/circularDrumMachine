cmake_minimum_required(VERSION 3.16)
project(circular_drum_machine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(CDM_BUILD_STANDALONE "Build the SDL2/OpenGL standalone application" ON)
option(CDM_BUILD_VST3 "Build the VST3 plugin (needs the VST3 SDK)" ON)
set(CDM_VST3SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vst3sdk"
    CACHE PATH "Root of the Steinberg VST3 SDK")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)

# --- engine: pattern clock and sample mixer, no windowing of any kind --------
add_library(cdm_engine STATIC src/audio.c)
target_include_directories(cdm_engine PUBLIC src vendor/dr_libs)
target_link_libraries(cdm_engine PUBLIC Threads::Threads)
if(NOT WIN32)
  target_link_libraries(cdm_engine PUBLIC m)
endif()

# --- interface: software canvas and the machine's ui ------------------------
add_library(cdm_ui STATIC src/draw.c src/ui.c)
target_link_libraries(cdm_ui PUBLIC cdm_engine PkgConfig::FREETYPE)
if(WIN32)
  # ui.c browses directories with <dirent.h>; src/compat supplies it there.
  target_include_directories(cdm_ui PRIVATE src/compat)
endif()

# --- standalone -------------------------------------------------------------
if(CDM_BUILD_STANDALONE)
  pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)
  # libpng ships as libpng.pc on Debian and as libpng16.pc on some mingw setups.
  pkg_search_module(LIBPNG REQUIRED IMPORTED_TARGET libpng libpng16)
  find_package(OpenGL REQUIRED)

  # Prefer a system SDL2. Otherwise use the libsdl2-dev package unpacked under
  # vendor/ (same version as the installed runtime, no root required):
  #   cd vendor && apt-get download libsdl2-dev && dpkg -x libsdl2-dev_*.deb sdl2/sdklocal
  pkg_search_module(SDL2 QUIET IMPORTED_TARGET sdl2 SDL2)
  if(SDL2_FOUND)
    set(SDL2_TARGET PkgConfig::SDL2)
  elseif(WIN32)
    message(FATAL_ERROR "SDL2 not found: install the mingw SDL2 package")
  else()
    set(SDL2_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl2/sdklocal/usr")
    if(NOT EXISTS "${SDL2_VENDOR}/include/SDL2/SDL.h")
      message(FATAL_ERROR
        "SDL2 headers not found. Install libsdl2-dev, or unpack it into vendor/sdl2/sdklocal")
    endif()
    add_library(sdl2_vendor INTERFACE)
    target_include_directories(sdl2_vendor INTERFACE
      "${SDL2_VENDOR}/include/SDL2"
      "${SDL2_VENDOR}/include/x86_64-linux-gnu")
    find_file(SDL2_RUNTIME NAMES libSDL2-2.0.so.0
              PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib /usr/lib)
    if(NOT SDL2_RUNTIME)
      message(FATAL_ERROR "libSDL2-2.0.so.0 runtime not found")
    endif()
    target_link_libraries(sdl2_vendor INTERFACE "${SDL2_RUNTIME}")
    set(SDL2_TARGET sdl2_vendor)
  endif()

  add_executable(circular_drum_machine src/main.c src/audio_sdl.c)
  target_link_libraries(circular_drum_machine PRIVATE
    cdm_ui
    ${SDL2_TARGET}
    PkgConfig::GLEW
    PkgConfig::LIBPNG
    OpenGL::GL
    Threads::Threads)
  target_compile_options(circular_drum_machine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# --- VST3 plugin ------------------------------------------------------------
if(CDM_BUILD_VST3)
  if(NOT EXISTS "${CDM_VST3SDK_DIR}/pluginterfaces/vst/vsttypes.h")
    message(FATAL_ERROR
      "VST3 SDK not found at ${CDM_VST3SDK_DIR}.\n"
      "Run scripts/fetch-vst3sdk.sh, or set -DCDM_VST3SDK_DIR=<sdk>, "
      "or configure with -DCDM_BUILD_VST3=OFF for a standalone-only build.")
  endif()

  # Only the SDK libraries: its sample hosts want GTK, and we bring our own UI.
  set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
  if(WIN32)
    # The SDK's validator is a GUI-subsystem executable whose entry point is
    # main(), which mingw refuses to link. It is only a test host, but
    # enabling it also makes our plugin target depend on it.
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
    # A single .vst3 DLL instead of a bundle: the bundle's post-build step runs
    # moduleinfotool on the module from inside ninja, where it is probed before
    # it has been copied into Contents/x86_64-win. Linux keeps the bundle.
    set(SMTG_CREATE_BUNDLE_FOR_WINDOWS OFF CACHE BOOL "" FORCE)
  endif()
  add_subdirectory("${CDM_VST3SDK_DIR}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)

  set(CDM_VST3_SOURCES
    src/vst3/plugin.cpp
    # The SDK keeps this base class out of its "sdk" library; every
    # single-component plug-in compiles it itself.
    "${CDM_VST3SDK_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp")
  if(WIN32)
    list(APPEND CDM_VST3_SOURCES src/vst3/view_win.cpp)
    list(APPEND CDM_VST3_SOURCES
      "${CDM_VST3SDK_DIR}/public.sdk/source/main/dllmain.cpp")
  else()
    list(APPEND CDM_VST3_SOURCES src/vst3/view_x11.cpp)
    list(APPEND CDM_VST3_SOURCES
      "${CDM_VST3SDK_DIR}/public.sdk/source/main/linuxmain.cpp")
  endif()

  smtg_add_vst3plugin(circular_drum_machine_vst3 ${CDM_VST3_SOURCES})
  # "sdk" carries the plug-in base classes, the factory and the SDK include
  # path; the interface library brings the machine's own canvas and ui.
  target_link_libraries(circular_drum_machine_vst3 PRIVATE sdk cdm_ui)
  if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(circular_drum_machine_vst3 PRIVATE X11::X11)
  endif()
  if(WIN32)
    # One file a host can load on its own: the C++ runtime and freetype come in
    # statically, so nothing has to be installed next to the plug-in.
    target_link_options(circular_drum_machine_vst3 PRIVATE -static)
  endif()
endif()
