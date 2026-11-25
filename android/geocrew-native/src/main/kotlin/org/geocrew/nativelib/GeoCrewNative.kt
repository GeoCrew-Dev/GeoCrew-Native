package org.geocrew.nativelib

/**
 * Entry points into libgeocrew_native.so (GDAL, PROJ, GEOS).
 *
 * Spike API: enough to load the library and run GDAL algorithms. The real API
 * comes with the app rewrite.
 */
object GeoCrewNative {
    init {
        System.loadLibrary("geocrew_native")
    }

    /** "GDAL x, PROJ y, GEOS z". */
    @JvmStatic external fun versions(): String

    /**
     * Points PROJ and GDAL at [dataDir], which must hold the files of this AAR's
     * `assets/geocrew-native/` (proj.db and GDAL's data), and registers GDAL's
     * drivers. Call once.
     */
    @JvmStatic external fun setup(dataDir: String)

    /** Runs `gdal <args>`, returning its text output, or null on failure. */
    @JvmStatic external fun run(args: Array<String>): String?
}
