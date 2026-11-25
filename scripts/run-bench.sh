#!/usr/bin/env bash
# Runs a bench program on the connected phone or emulator through adb, using
# the stripped libraries and data files from scripts/stage-android.sh.
# Usage: scripts/run-bench.sh <triplet> [program [args...]]
#   default program: geocrew_bench, run on its own directory on the device
#   e.g. scripts/run-bench.sh x64-android geocrew_storage /sdcard/GeoCrew-spike
# Arguments must not contain spaces. Set ANDROID_SERIAL when several devices are attached.
set -euo pipefail
export MSYS_NO_PATHCONV=1 # Git Bash on Windows would rewrite the device paths

triplet=${1:?usage: scripts/run-bench.sh <triplet> [program [args...]]}
shift
program=${1:-geocrew_bench}
[ $# -gt 0 ] && shift
args=("$@")
[ ${#args[@]} -gt 0 ] || args=(.)
case $triplet in
arm64-android) abi=arm64-v8a ;;
x64-android) abi=x86_64 ;;
*) echo "unknown triplet $triplet" >&2; exit 2 ;;
esac
build=build/$triplet
staged=build/android
[ -d "$staged/jniLibs/$abi" ] || { echo "run scripts/stage-android.sh $triplet first" >&2; exit 2; }
ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}
ndk=$(printf '%s' "$ndk" | tr '\\' /) # Windows paths use backslashes
strip=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip* | head -n 1)
"$strip" --strip-unneeded -o "$build/$program.stripped" "$build/$program"
dir=/data/local/tmp/geocrew-bench

adb shell "rm -rf $dir && mkdir -p $dir"
adb push -q "$build/$program.stripped" "$dir/$program"
adb push -q "$staged/jniLibs/$abi"/*.so "$dir/"
adb push -q "$staged/assets/geocrew-native" "$dir/data"
adb shell "cd $dir && chmod 755 $program && LD_LIBRARY_PATH=. PROJ_DATA=data GDAL_DATA=data ./$program ${args[*]}"
