# GeoCrew's arm64-osx triplet for the server's GDAL helper (ADR 0013 in the main repository):
# static libraries, release builds only. It overrides vcpkg's own arm64-osx.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 12.0)
set(VCPKG_BUILD_TYPE release)
