@echo off
setlocal
if not defined PLATFORM set PLATFORM=x64
call buildwin\win_deps.bat || exit /b 1
cmake -S . -B build -A x64 -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release -DwxWidgets_LIB_DIR=%wxWidgets_LIB_DIR% -DwxWidgets_ROOT_DIR=%wxWidgets_ROOT_DIR% "-DOCPN_TARGET_TUPLE=msvc-64;10;x86_64" || exit /b 1
cmake --build build --target tarball --config Release || exit /b 1
