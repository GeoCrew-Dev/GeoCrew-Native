#!/usr/bin/env bash
# Runs geocrew_bench on the connected phone or emulator through adb, using the
# stripped libraries and data files from scripts/stage-android.sh.
# Usage: scripts/run-bench.sh <triplet>   (x64-android for the emulator, arm64-android for phones)
set -euo pipefail
export MSYS_NO_PATHCONV=1 # Git Bash on Windows would rewrite the device paths

triplet=${1:?usage: scripts/run-bench.sh <triplet>}
case $triplet in
arm64-android) abi=arm64-v8a ;;
x64-android) abi=x86_64 ;;
*) echo "unknown triplet $triplet" >&2; exit 2 ;;
esac
build=build/$triplet
staged=build/android
[ -d "$staged/jniLibs/$abi" ] || { echo "run scripts/stage-android.sh $triplet first" >&2; exit 2; }
ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}
ndk=${ndk//\//}
strip=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip* | head -n 1)
"$strip" --strip-unneeded -o "$build/geocrew_bench.stripped" "$build/geocrew_bench"
dir=/data/local/tmp/geocrew-bench

adb shell "rm -rf $dir && mkdir -p $dir"
adb push -q "$build/geocrew_bench.stripped" "$dir/geocrew_bench"
adb push -q "$staged/jniLibs/$abi"/*.so "$dir/"
adb push -q "$staged/assets/geocrew-native" "$dir/data"
adb shell "cd $dir && chmod 755 geocrew_bench && LD_LIBRARY_PATH=. PROJ_DATA=data GDAL_DATA=data ./geocrew_bench ."
