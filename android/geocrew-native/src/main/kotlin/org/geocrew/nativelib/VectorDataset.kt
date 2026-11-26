package org.geocrew.nativelib

/**
 * A vector dataset (a GeoPackage, a Shapefile, anything GDAL reads) kept open
 * and read and written feature by feature (ADR 0004 and 0005 in the main
 * repository). Call [GeoCrewNative.setup] first.
 *
 * Features are JSON, in the layer's own CRS:
 * `{"fid": 1, "geometry": <GeoJSON geometry or null>, "properties": {"name": value}}`.
 * Coordinates and real numbers read back exactly as written; dates are ISO 8601
 * strings. Writing a feature changes only the members and properties it names.
 *
 * One call runs at a time across all datasets. Failures throw
 * [IllegalArgumentException] for bad input and [RuntimeException] with GDAL's
 * message otherwise; any call after [close] throws [IllegalStateException].
 */
class VectorDataset private constructor(handle: Long) : AutoCloseable {
    @Volatile private var handle = handle

    companion object {
        /**
         * Opens the dataset at [path], for writing when [update]. [wal] puts a
         * SQLite-based file (GeoPackage) in WAL mode, which stays in the file:
         * for the app's own projects, not for other people's files.
         */
        fun open(path: String, update: Boolean = false, wal: Boolean = false) =
            VectorDataset(VectorNative.open(path, update, wal))

        /** Creates a new, empty dataset at [path] with GDAL driver [driver]; [path] must not exist. */
        fun create(path: String, driver: String = "GPKG", wal: Boolean = true) =
            VectorDataset(VectorNative.create(path, driver, wal))
    }

    /** The names of the layers. */
    val layers: List<String> get() = VectorNative.layerNames(h()).toList()

    /**
     * JSON: name, geometryType ("POINT", "LINESTRING Z", ...), srs ("EPSG:4547",
     * WKT2 for a CRS without a code, or null), count, fields (name and type) and
     * capabilities (randomWrite, deleteFeature, createField, transactions).
     */
    fun layerInfo(layer: String): String = VectorNative.layerInfo(h(), layer)

    /**
     * Adds layer [name] with [geometryType] (OGC name, such as "POINT" or
     * "LINESTRING Z"), [srs] (anything GDAL understands, such as "EPSG:4547"; null
     * for none) and [fields], a JSON array of `{"name": ..., "type": ...}` with
     * type integer, integer64, real, string, date, datetime or boolean.
     */
    fun createLayer(name: String, geometryType: String, srs: String?, fields: String = "[]") =
        VectorNative.createLayer(h(), name, geometryType, srs, fields)

    /**
     * The layer's features as a JSON array: all of them, or those matching
     * [where] (an OGR SQL condition) and [bbox] (minX, minY, maxX, maxY in the
     * layer's CRS).
     */
    fun features(layer: String, where: String? = null, bbox: DoubleArray? = null): String =
        VectorNative.features(h(), layer, where, bbox)

    /** Feature [fid] as JSON, or null when there is none. */
    fun feature(layer: String, fid: Long): String? = VectorNative.feature(h(), layer, fid)

    /** Adds [feature] and returns its fid. */
    fun add(layer: String, feature: String): Long = VectorNative.add(h(), layer, feature)

    /** Changes feature [fid]: its geometry, if [feature] has one, and the properties [feature] names. */
    fun update(layer: String, fid: Long, feature: String) = VectorNative.update(h(), layer, fid, feature)

    fun delete(layer: String, fid: Long) = VectorNative.delete(h(), layer, fid)

    /** Starts a transaction, for drivers that have them (layerInfo's capabilities.transactions). */
    fun begin() = VectorNative.transaction(h(), 0)

    fun commit() = VectorNative.transaction(h(), 1)

    fun rollback() = VectorNative.transaction(h(), 2)

    /** Runs [statement] in the dataset's SQL dialect (SQLite for a GeoPackage); its rows as features, as [features] returns them. */
    fun sql(statement: String): String = VectorNative.sql(h(), statement)

    override fun close() {
        val h = handle
        handle = 0
        if (h != 0L) VectorNative.close(h)
    }

    private fun h(): Long = handle.also { check(it != 0L) { "the dataset is closed" } }
}

internal object VectorNative {
    init {
        System.loadLibrary("geocrew_native")
    }

    @JvmStatic external fun open(path: String, update: Boolean, wal: Boolean): Long
    @JvmStatic external fun create(path: String, driver: String, wal: Boolean): Long
    @JvmStatic external fun close(handle: Long)
    @JvmStatic external fun layerNames(handle: Long): Array<String>
    @JvmStatic external fun layerInfo(handle: Long, layer: String): String
    @JvmStatic external fun createLayer(handle: Long, name: String, geometryType: String, srs: String?, fields: String)
    @JvmStatic external fun features(handle: Long, layer: String, where: String?, bbox: DoubleArray?): String
    @JvmStatic external fun feature(handle: Long, layer: String, fid: Long): String?
    @JvmStatic external fun add(handle: Long, layer: String, feature: String): Long
    @JvmStatic external fun update(handle: Long, layer: String, fid: Long, feature: String)
    @JvmStatic external fun delete(handle: Long, layer: String, fid: Long)
    @JvmStatic external fun transaction(handle: Long, what: Int)
    @JvmStatic external fun sql(handle: Long, statement: String): String
}
