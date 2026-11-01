@echo off
setlocal
:: PLATFORM=x64 builds for 64-bit OpenCPN 5.14+ (msvc-64); PLATFORM=Win32
:: builds for the 32-bit OpenCPN 5.8-5.12 (msvc-wx32). OpenCPN refuses a
:: package whose target does not match its own build.
if not defined PLATFORM set PLATFORM=x64
call buildwin\win_deps.bat || exit /b 1
if /I "%PLATFORM%"=="x64" (
  set "CMAKE_ARCH=x64"
  set "TARGET_TUPLE=msvc-64;10;x86_64"
) else (
  set "CMAKE_ARCH=Win32"
  set "TARGET_TUPLE=msvc-wx32;10;x86"
)
cmake -S . -B build -A %CMAKE_ARCH% -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release -DwxWidgets_LIB_DIR=%wxWidgets_LIB_DIR% -DwxWidgets_ROOT_DIR=%wxWidgets_ROOT_DIR% "-DOCPN_TARGET_TUPLE=%TARGET_TUPLE%" || exit /b 1
cmake --build build --target tarball --config Release || exit /b 1
