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

    /**
     * Runs `gdal <args>` and returns its text output (empty for algorithms that
     * only write files). Throws [RuntimeException] with GDAL's error message
     * when the arguments are wrong or the algorithm fails.
     */
    @JvmStatic external fun run(args: Array<String>): String

    // Map spike (M1): what CustomGeometrySource needs. Datasets stay open
    // between calls; one lock serialises calls from MapLibre's threads.

    /**
     * Runs [sql] on the GeoPackage at [path]. The query must return a point
     * geometry and an integer class as its first field. Returns a [DoubleArray]
     * of x0, y0, x1, y1, ... and an [IntArray] of the classes.
     */
    @JvmStatic external fun queryPoints(path: String, sql: String): Array<Any>

    /** Moves point feature [fid] of [layer] in the GeoPackage at [path] to ([x], [y]). */
    @JvmStatic external fun movePoint(path: String, layer: String, fid: Long, x: Double, y: Double)

    /** Closes the datasets opened by [queryPoints] and [movePoint]; later calls to them fail. */
    @JvmStatic external fun closeAll()

    // Raster spike (M1): rasters in any CRS warped to Web Mercator tiles on request.

    /**
     * Renders Web Mercator tile [z]/[x]/[y] (XYZ, 256 px) of the raster at [path]
     * as PNG, warped from the raster's own CRS; an empty array for a tile the
     * raster does not reach.
     */
    @JvmStatic external fun renderTile(path: String, z: Int, x: Int, y: Int): ByteArray

    /** Writes a 40 km test GeoTIFF in EPSG:4547 around 114E 30N, with a line every kilometre. */
    @JvmStatic external fun makeTestRaster(path: String)
}
