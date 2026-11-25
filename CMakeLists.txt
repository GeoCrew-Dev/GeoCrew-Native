cmake_minimum_required(VERSION 3.25)
project(geocrew_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GDAL CONFIG REQUIRED)
find_package(PROJ CONFIG REQUIRED)
find_package(GEOS CONFIG REQUIRED)

if(ANDROID)
    # The library the app loads. GDAL, PROJ and their static dependencies are linked
    # in with their symbols hidden; GEOS and GNU libiconv (LGPL) stay separate .so files.
    add_library(geocrew_native SHARED src/geocrew_jni.cpp)
    target_link_libraries(geocrew_native PRIVATE GDAL::GDAL PROJ::proj GEOS::geos_c)
    target_link_options(geocrew_native PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections)

    # Benchmark run on a device through adb, see bench/README.md.
    add_executable(geocrew_bench bench/geocrew_bench.cpp)
    target_link_libraries(geocrew_bench PRIVATE GDAL::GDAL PROJ::proj GEOS::geos_c)

    # GeoPackage speed and crash recovery in given directories, see bench/README.md.
    add_executable(geocrew_storage bench/geocrew_storage.cpp)
    target_link_libraries(geocrew_storage PRIVATE GDAL::GDAL)
else()
    # The server's GDAL helper, one self-contained program per platform (ADR 0013
    # in the main repository). Only the system's C library stays dynamic.
    add_executable(geocrew-gdal-helper helper/geocrew_gdal_helper.cpp)
    target_link_libraries(geocrew-gdal-helper PRIVATE GDAL::GDAL PROJ::proj GEOS::geos_c)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(geocrew-gdal-helper PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()
