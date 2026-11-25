#!/usr/bin/env bash
# Checks the staged libraries in build/android/jniLibs and reports their sizes:
# every LOAD segment must be aligned to 16 KB, which Android 15+ devices with
# 16 KB pages require. Writes the size table to $GITHUB_STEP_SUMMARY in CI.
set -euo pipefail

ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}
ndk=$(printf '%s' "$ndk" | tr '\\' /) # Windows paths use backslashes
readelf=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-readelf* | head -n 1)
status=0
table="| ABI | Library | Size (KB) | gzip -9 (KB) |
|---|---|---:|---:|"
for dir in build/android/jniLibs/*/; do
    abi=$(basename "$dir")
    total=0 ztotal=0
    for so in "$dir"*.so; do
        for align in $("$readelf" -lW "$so" | awk '$1 == "LOAD" { print $NF }'); do
            if (( align < 16384 )); then
                echo "$abi/$(basename "$so"): LOAD segment aligned to $align, needs 16 KB" >&2
                status=1
            fi
        done
        size=$(wc -c < "$so") zsize=$(gzip -9c "$so" | wc -c)
        total=$((total + size)) ztotal=$((ztotal + zsize))
        table+="
| $abi | $(basename "$so") | $((size / 1024)) | $((zsize / 1024)) |"
    done
    table+="
| $abi | **total** | **$((total / 1024))** | **$((ztotal / 1024))** |"
done
data=build/android/assets/geocrew-native
db=$data/proj.db
gdal_files=$(find "$data" -type f ! -name proj.db | sort)
table+="
| | proj.db | $(($(wc -c < "$db") / 1024)) | $(($(gzip -9c "$db" | wc -c) / 1024)) |
| | GDAL data, $(echo "$gdal_files" | wc -l) files | $(($(cat $gdal_files | wc -c) / 1024)) | $(($(cat $gdal_files | gzip -9 | wc -c) / 1024)) |"
echo "$table"
[ -z "${GITHUB_STEP_SUMMARY:-}" ] || echo "$table" >> "$GITHUB_STEP_SUMMARY"
exit $status
