# GeoCrew's x64-linux triplet for the server's GDAL helper (ADR 0013 in the main repository):
# static libraries, release builds only. It overrides vcpkg's own x64-linux.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
