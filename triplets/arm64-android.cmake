# GeoCrew's arm64-android triplet. It overrides vcpkg's built-in one:
# - API level 31, the app's minSdk (Android 12).
# - Release builds only; the app never ships debug builds of these libraries.
# - Shared libc++ (vcpkg picks c++_shared for a dynamic CRT): more than one .so uses C++.
# - 16 KB page alignment, which Android 15+ devices with 16 KB pages require.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 31)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a)
set(VCPKG_LINKER_FLAGS "-Wl,-z,max-page-size=16384")
set(VCPKG_BUILD_TYPE release)

# LGPL libraries are separate shared libraries, so users can replace them.
if(PORT STREQUAL "geos" OR PORT STREQUAL "libiconv")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

# Android's own iconv (bionic) only converts UTF-8 and UTF-16; GBK shapefiles need GNU libiconv.
# CMake's FindIconv would still pick the libc one, which exists from API 28.
set(X_VCPKG_BUILD_GNU_LIBICONV 1)
if(PORT STREQUAL "gdal")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DIconv_IS_BUILT_IN=FALSE)
endif()
