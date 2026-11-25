#!/usr/bin/env bash
# Copies the CMake builds into build/android/, where Gradle packages them:
#   jniLibs/<abi>/*.so       stripped libraries for the AAR
#   symbols/<abi>/*.so       the same libraries unstripped, for reading crash stacks
#   assets/geocrew-native/   proj.db and GDAL's data files (GDAL_DATA)
# Usage: scripts/stage-android.sh [triplet...]   (default: arm64-android x64-android)
set -euo pipefail

ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}
ndk=${ndk//\//}
strip=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip* | head -n 1)
out=build/android
rm -rf "$out"
mkdir -p "$out/assets/geocrew-native"

triplets=("$@")
[ ${#triplets[@]} -gt 0 ] || triplets=(arm64-android x64-android)
for triplet in "${triplets[@]}"; do
    case $triplet in
    arm64-android) abi=arm64-v8a target=aarch64-linux-android ;;
    x64-android) abi=x86_64 target=x86_64-linux-android ;;
    *) echo "unknown triplet $triplet" >&2; exit 2 ;;
    esac
    installed=build/$triplet/vcpkg_installed/$triplet
    mkdir -p "$out/jniLibs/$abi" "$out/symbols/$abi"
    for so in "build/$triplet/libgeocrew_native.so" "$installed"/lib/*.so \
        "$ndk"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/$target/libc++_shared.so; do
        cp "$so" "$out/symbols/$abi/"
        "$strip" --strip-unneeded -o "$out/jniLibs/$abi/$(basename "$so")" "$so"
    done
    cp "$installed/share/proj/proj.db" "$out/assets/geocrew-native/"
    # GDAL's data files; share/gdal also holds vcpkg's CMake files and metadata.
    find "$installed/share/gdal" -maxdepth 1 -type f ! -name '*.cmake' ! -name 'vcpkg*' \
        ! -name copyright ! -name usage ! -name LICENSE.TXT -exec cp {} "$out/assets/geocrew-native/" \;
done
