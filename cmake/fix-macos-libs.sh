#!/bin/bash

# Adjust absolute paths to wxWidgets libraries to match their
# location ot the target MacOS system where they are installed
# as part of OpenCPN

readonly RUNTIME_PATH="@executable_path/../Frameworks/"

plugin=$(find "${1:-app/files}" -name '*.dylib')
if [ -z "$plugin" ]; then
    echo "fix-macos-libs.sh: no plugin library found" >&2
    exit 1
fi

for lib in $(otool -L "$plugin" | awk ' /wx/ {print $1}'); do
    libdir=${lib%/*}
    if [ "$libdir" = "$lib" ]; then
        continue
    elif [ "$libdir" != "$RUNTIME_PATH" ]; then
        runtime_lib=$(echo $lib | sed "s|$libdir|$RUNTIME_PATH|")
        install_name_tool -change "$lib" "$runtime_lib" "$plugin"
    fi
done
# The changes above invalidate the signature; an arm64 library must be signed.
codesign --force --sign - "$plugin"
echo "fix-macos-libs.sh: Revised library paths:"
otool -L "$plugin" | grep wx
