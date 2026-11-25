cmake_minimum_required(VERSION 3.25)
project(geocrew_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GDAL CONFIG REQUIRED)
find_package(PROJ CONFIG REQUIRED)
find_package(GEOS CONFIG REQUIRED)

# The library the app loads. GDAL, PROJ and their static dependencies are linked
# in with their symbols hidden; GEOS and GNU libiconv (LGPL) stay separate .so files.
add_library(geocrew_native SHARED src/geocrew_jni.cpp)
target_link_libraries(geocrew_native PRIVATE GDAL::GDAL PROJ::proj GEOS::geos_c)
target_link_options(geocrew_native PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections)

# Benchmark run on a device through adb, see bench/README.md.
add_executable(geocrew_bench bench/geocrew_bench.cpp)
target_link_libraries(geocrew_bench PRIVATE GDAL::GDAL PROJ::proj GEOS::geos_c)
