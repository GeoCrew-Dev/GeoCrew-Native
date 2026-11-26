package org.geocrew.nativelib

/**
 * Coordinates between CRSs, through PROJ: positions from GNSS and the map
 * (CGCS2000 longitude and latitude, EPSG:4490) to a layer's CRS and back.
 *
 * A CRS is named as GDAL reads it: EPSG:4549, a WKT or a PROJ string. x is the
 * longitude or easting and y the latitude or northing, whatever the CRS's own
 * axis order. Call [GeoCrewNative.setup] first.
 */
object Coordinates {
    init {
        System.loadLibrary("geocrew_native")
    }

    /**
     * [xy] (x0, y0, x1, y1, ...) from CRS [from] to CRS [to]. A pair PROJ cannot
     * transform comes back as NaN, NaN. Throws [IllegalArgumentException] for a
     * CRS GDAL does not know, a pair of CRSs it cannot transform between, or an
     * odd number of values.
     *
     * Between datums PROJ knows no transformation for, it uses a "ballpark" one
     * that shifts nothing and can be metres off (Xian 1980 to CGCS2000, say);
     * CGCS2000 and WGS84 are within centimetres, as ADR 0015 takes them.
     */
    @JvmStatic external fun transform(from: String, to: String, xy: DoubleArray): DoubleArray

    /**
     * What [srs] is, as JSON: its name, its id (such as EPSG:4549) when it has one,
     * whether it is geographic or projected, and its unit ("degree", "metre").
     * Throws [IllegalArgumentException] for a CRS GDAL does not know.
     */
    @JvmStatic external fun describe(srs: String): String
}
