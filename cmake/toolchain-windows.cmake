# Cross-compilation to Windows x86_64 with mingw-w64.
#
#   cmake -S . -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows.cmake
#
# The dependencies (SDL2, freetype, libpng, GLEW) have to be mingw builds; point
# CDM_WINDOWS_DEPS at a prefix containing their include/ and lib/ directories,
# or just build inside msys2, where pkg-config finds them (see BUILDING.md and
# .github/workflows/build.yml, which is how the Windows artifacts are made).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(DEFINED CDM_WINDOWS_DEPS)
  set(CMAKE_PREFIX_PATH "${CDM_WINDOWS_DEPS}" CACHE PATH "" FORCE)
  set(ENV{PKG_CONFIG_PATH} "${CDM_WINDOWS_DEPS}/lib/pkgconfig")
  set(ENV{PKG_CONFIG_LIBDIR} "${CDM_WINDOWS_DEPS}/lib/pkgconfig")
endif()
