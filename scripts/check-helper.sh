#!/usr/bin/env bash
# Stages and checks the server's GDAL helper built for one desktop triplet:
#   build/helper/<triplet>/geocrew-gdal-helper[.exe]
#   build/helper/<triplet>/data/   proj.db and GDAL's data files
# Then runs it: --version, a reprojecting conversion with a Chinese attribute
# read back, and lists what it links dynamically (on Linux, the newest glibc it
# needs). Usage: scripts/check-helper.sh <triplet>
set -euo pipefail

triplet=${1:?usage: scripts/check-helper.sh <triplet>}
build=build/$triplet
installed=$build/vcpkg_installed/$triplet
exe=$(ls "$build"/geocrew-gdal-helper "$build"/Release/geocrew-gdal-helper.exe 2>/dev/null | head -n 1 || true)
[ -n "$exe" ] || { echo "no helper built in $build" >&2; exit 2; }
out=build/helper/$triplet
rm -rf "$out"
mkdir -p "$out/data"
cp "$exe" "$out/"
cp "$installed/share/proj/proj.db" "$out/data/"
# GDAL's data files; share/gdal also holds vcpkg's CMake files and metadata.
find "$installed/share/gdal" -maxdepth 1 -type f ! -name '*.cmake' ! -name 'vcpkg*' \
    ! -name copyright ! -name usage ! -name LICENSE.TXT -exec cp {} "$out/data/" \;
helper=$out/$(basename "$exe")

"$helper" --version
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf '%s' '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{"rock":"花岗岩露头"},"geometry":{"type":"Point","coordinates":[114.0,30.0]}}]}' > "$work/in.geojson"
"$helper" vector pipeline read "$work/in.geojson" ! reproject --dst-crs EPSG:4547 ! write "$work/out.gpkg"
info=$("$helper" vector info --format=json --features "$work/out.gpkg")
# 114E 30N on the 114E central meridian: northing about 3320113.4 m.
grep -q 3320113 <<< "$info" || { echo "the reprojected point is wrong:"; echo "$info"; exit 1; }
grep -q '花岗岩露头' <<< "$info" || { echo "the Chinese attribute was lost:"; echo "$info"; exit 1; }
echo "conversion and read-back OK"
echo "helper: $(($(wc -c < "$helper") / 1048576)) MB, data: $(($(cat "$out"/data/* | wc -c) / 1048576)) MB"
case $(uname -s) in
Linux)
    ldd "$helper"
    echo "needs $(objdump -T "$helper" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)"
    ;;
Darwin) otool -L "$helper" ;;
MINGW* | MSYS*)
    vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    vs=$("$vswhere" -latest -products '*' -property installationPath | tr -d '')
    dumpbin=$(ls "$(cygpath -u "$vs")"/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe | head -n 1)
    "$dumpbin" //nologo //dependents "$(cygpath -w "$helper")" | grep -i '\.dll'
    ;;
*) echo "dynamic dependencies are not listed on this system" ;;
esac
