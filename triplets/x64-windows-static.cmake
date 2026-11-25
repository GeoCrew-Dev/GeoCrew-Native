# GeoCrew's x64-windows-static triplet for the server's GDAL helper (ADR 0013 in the main
# repository): static libraries and C runtime, release builds only. It overrides vcpkg's own.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
