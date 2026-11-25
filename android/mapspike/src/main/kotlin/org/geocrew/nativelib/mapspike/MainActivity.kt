package org.geocrew.nativelib.mapspike

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.widget.FrameLayout
import android.widget.TextView
import java.io.File
import kotlin.random.Random
import org.geocrew.nativelib.GeoCrewNative
import org.maplibre.android.MapLibre
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.geometry.LatLngBounds
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.style.expressions.Expression
import org.maplibre.android.style.layers.CircleLayer
import org.maplibre.android.style.layers.PropertyFactory
import org.maplibre.android.style.sources.CustomGeometrySource
import org.maplibre.android.style.sources.CustomGeometrySourceOptions
import org.maplibre.android.style.sources.GeometryTileProvider
import org.maplibre.geojson.Feature
import org.maplibre.geojson.FeatureCollection
import org.maplibre.geojson.Point

/**
 * M1 spike: 100k GeoPackage points drawn through CustomGeometrySource.
 *
 * On start it builds the GeoPackage if needed, shows the whole area, then runs
 * the measurements and logs them under the tag "GeoCrewMap":
 * - time until the first full render with every point on screen;
 * - every tile query (rows, query time, time to build MapLibre features);
 * - frame rate while zooming in and out;
 * - at zoom 14, time from moving one point to the map showing it again (10 edits).
 */
class MainActivity : Activity() {
    private lateinit var mapView: MapView
    private lateinit var status: TextView
    private lateinit var gpkg: File
    private lateinit var source: CustomGeometrySource
    private lateinit var map: MapLibreMap
    private val report = StringBuilder()
    private val stats = TileStats()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MapLibre.getInstance(this)
        mapView = MapView(this)
        status = TextView(this).apply {
            setBackgroundColor(0xCCFFFFFF.toInt())
            textSize = 11f
        }
        setContentView(FrameLayout(this).apply {
            addView(mapView)
            addView(status)
        })
        mapView.onCreate(savedInstanceState)
        Thread {
            prepare()
            runOnUiThread { mapView.getMapAsync(::start) }
        }.start()
    }

    /** Unpacks proj.db and GDAL's data, and writes 100k points into a GeoPackage once. */
    private fun prepare() {
        val dataDir = File(filesDir, "geocrew-native").apply { mkdirs() }
        for (name in assets.list("geocrew-native").orEmpty()) {
            val target = File(dataDir, name)
            if (!target.exists()) assets.open("geocrew-native/$name").use { input -> target.outputStream().use { input.copyTo(it) } }
        }
        GeoCrewNative.setup(dataDir.path)
        gpkg = File(filesDir, "points.gpkg")
        if (gpkg.exists()) return
        val t0 = SystemClock.elapsedRealtime()
        val geojson = File(cacheDir, "points.geojson")
        val random = Random(42)
        geojson.bufferedWriter().use { w ->
            w.write("""{"type":"FeatureCollection","features":[""")
            for (i in 0 until POINTS) {
                if (i > 0) w.write(",")
                val x = 113.5 + random.nextDouble()
                val y = 29.5 + random.nextDouble()
                val elev = 100 + random.nextInt(900)
                w.write("""{"type":"Feature","properties":{"elev":$elev},"geometry":{"type":"Point","coordinates":[$x,$y]}}""")
            }
            w.write("]}")
        }
        GeoCrewNative.run(arrayOf("vector", "convert", geojson.path, gpkg.path))
        geojson.delete()
        log("built the GeoPackage with $POINTS points in ${SystemClock.elapsedRealtime() - t0} ms")
    }

    private fun start(map: MapLibreMap) {
        this.map = map
        map.setStyle(Style.Builder().fromJson(STYLE)) { style ->
            source = CustomGeometrySource("points", CustomGeometrySourceOptions().withMaxZoom(16), Provider())
            style.addSource(source)
            style.addLayer(
                CircleLayer("points", "points").withProperties(
                    PropertyFactory.circleRadius(2.5f),
                    PropertyFactory.circleColor(
                        Expression.match(
                            Expression.get("cls"),
                            Expression.color(Color.parseColor("#2c7fb8")),
                            Expression.stop(1, Expression.color(Color.parseColor("#d7301f"))),
                        ),
                    ),
                ),
            )
            measure(map)
        }
    }

    private fun measure(map: MapLibreMap) {
        map.cameraPosition = CameraPosition.Builder().target(LatLng(30.0, 114.0)).zoom(8.0).build()
        val t0 = SystemClock.elapsedRealtime()
        whenFullyRendered {
            log("first full render at zoom 8 (all $POINTS points): ${SystemClock.elapsedRealtime() - t0} ms")
            log(stats.drain("tile queries for the first render"))
            zoomTest(map) { editTest(map) }
        }
    }

    /** Zooms 8 → 12 → 8 → 15 → 8, two seconds each, and reports the frame rate. */
    private fun zoomTest(map: MapLibreMap, done: () -> Unit) {
        val zooms = listOf(12.0, 8.0, 15.0, 8.0)
        var frames = 0
        val listener = MapView.OnDidFinishRenderingFrameListener { _, _, _ -> frames++ }
        mapView.addOnDidFinishRenderingFrameListener(listener)
        val t0 = SystemClock.elapsedRealtime()
        fun step(i: Int) {
            if (i == zooms.size) {
                val seconds = (SystemClock.elapsedRealtime() - t0) / 1000.0
                mapView.removeOnDidFinishRenderingFrameListener(listener)
                log("zooming for %.1f s: %d frames, %.1f fps".format(seconds, frames, frames / seconds))
                log(stats.drain("tile queries while zooming"))
                // The map is idle now and would send no further "fully rendered" event.
                mapView.postDelayed(done, 1000)
                return
            }
            map.animateCamera(CameraUpdateFactory.zoomTo(zooms[i]), 2000, object : MapLibreMap.CancelableCallback {
                override fun onFinish() = step(i + 1)
                override fun onCancel() = step(i + 1)
            })
        }
        step(0)
    }

    /**
     * At zoom 14, where editing happens: brings point 1 to the centre, then moves
     * it [EDITS] times, each time refreshing only the region around its old and
     * new position. A refreshed region sends no "fully rendered" event, so each
     * move is timed until its tiles are queried again (no further tile for
     * 150 ms); the map draws them in the next frame.
     */
    private fun editTest(map: MapLibreMap) {
        map.cameraPosition = CameraPosition.Builder().target(LatLng(30.0, 114.0)).zoom(14.0).build()
        GeoCrewNative.movePoint(gpkg.path, "points", 1, 114.0, 30.0)
        source.invalidateRegion(LatLngBounds.Builder().include(LatLng(29.5, 113.5)).include(LatLng(30.5, 114.5)).build())
        whenFullyRendered {
            stats.drain("")
            editRound(0, 114.0, 30.0, mutableListOf(), mutableListOf())
        }
    }

    private fun editRound(round: Int, lastX: Double, lastY: Double, writes: MutableList<Long>, totals: MutableList<Long>) {
        if (round == EDITS) {
            log("edit at zoom 14, $EDITS rounds: writing the point median ${writes.sorted()[EDITS / 2]} ms; " +
                "until its tiles are queried again median ${totals.sorted()[EDITS / 2]} ms, max ${totals.max()} ms")
            log(stats.drain("tile queries for the edits"))
            log("done")
            return
        }
        val random = Random(round)
        val x = 114.0 + (random.nextDouble() - 0.5) * 0.004
        val y = 30.0 + (random.nextDouble() - 0.5) * 0.004
        val before = stats.tilesDone()
        val t0 = SystemClock.elapsedRealtime()
        GeoCrewNative.movePoint(gpkg.path, "points", 1, x, y)
        writes += SystemClock.elapsedRealtime() - t0
        val margin = 0.0005
        source.invalidateRegion(
            LatLngBounds.Builder()
                .include(LatLng(minOf(y, lastY) - margin, minOf(x, lastX) - margin))
                .include(LatLng(maxOf(y, lastY) + margin, maxOf(x, lastX) + margin))
                .build(),
        )
        val poll = object : Runnable {
            override fun run() {
                val now = SystemClock.elapsedRealtime()
                val settled = stats.tilesDone() > before && now - stats.lastTileDoneAt() >= 150
                if (settled || now - t0 > 5000) {
                    if (!settled) log("round $round: no tile was queried again within 5 s")
                    totals += if (settled) stats.lastTileDoneAt() - t0 else now - t0
                    editRound(round + 1, x, y, writes, totals)
                } else {
                    mapView.postDelayed(this, 10)
                }
            }
        }
        mapView.post(poll)
    }

    /** Calls [then] once, after the next fully rendered map, or after 5 seconds if none comes. */
    private fun whenFullyRendered(then: () -> Unit) {
        var called = false
        lateinit var listener: MapView.OnDidFinishRenderingMapListener
        val timeout = Runnable {
            if (!called) {
                called = true
                mapView.removeOnDidFinishRenderingMapListener(listener)
                log("no fully rendered map within 5 s")
                then()
            }
        }
        listener = MapView.OnDidFinishRenderingMapListener { fully ->
            if (fully && !called) {
                called = true
                mapView.removeOnDidFinishRenderingMapListener(listener)
                mapView.removeCallbacks(timeout)
                mapView.post(then)
            }
        }
        mapView.addOnDidFinishRenderingMapListener(listener)
        mapView.postDelayed(timeout, 5000)
        map.triggerRepaint()
    }

    private fun log(line: String) {
        Log.i(TAG, line)
        runOnUiThread {
            report.appendLine(line)
            status.text = report
        }
    }

    /** Queries the tile's bounding box through the spatial index; the class for the colour comes from SQL. */
    private inner class Provider : GeometryTileProvider {
        override fun getFeaturesForBounds(bounds: LatLngBounds, zoom: Int): FeatureCollection {
            val t0 = SystemClock.elapsedRealtimeNanos()
            val sql = "SELECT CASE WHEN p.elev > 500 THEN 1 ELSE 0 END AS cls, p.geom FROM points p " +
                "JOIN rtree_points_geom r ON p.fid = r.id " +
                "WHERE r.minx <= ${bounds.longitudeEast} AND r.maxx >= ${bounds.longitudeWest} " +
                "AND r.miny <= ${bounds.latitudeNorth} AND r.maxy >= ${bounds.latitudeSouth}"
            val result = GeoCrewNative.queryPoints(gpkg.path, sql)
            val xy = result[0] as DoubleArray
            val cls = result[1] as IntArray
            val t1 = SystemClock.elapsedRealtimeNanos()
            val features = ArrayList<Feature>(cls.size)
            for (i in cls.indices) {
                val feature = Feature.fromGeometry(Point.fromLngLat(xy[2 * i], xy[2 * i + 1]))
                feature.addNumberProperty("cls", cls[i])
                features += feature
            }
            stats.record(zoom, cls.size, (t1 - t0) / 1e6, (SystemClock.elapsedRealtimeNanos() - t1) / 1e6)
            return FeatureCollection.fromFeatures(features)
        }
    }

    /** Tile query statistics, collected from MapLibre's worker threads. */
    private class TileStats {
        private var tiles = 0
        private var rows = 0L
        private var maxRows = 0
        private var queryMs = 0.0
        private var buildMs = 0.0
        private var slowest = 0.0

        private var done = 0
        private var lastDoneAt = 0L

        @Synchronized
        fun tilesDone() = done

        @Synchronized
        fun lastTileDoneAt() = lastDoneAt

        @Synchronized
        fun record(zoom: Int, n: Int, query: Double, build: Double) {
            done++
            lastDoneAt = SystemClock.elapsedRealtime()
            tiles++
            rows += n
            maxRows = maxOf(maxRows, n)
            queryMs += query
            buildMs += build
            slowest = maxOf(slowest, query + build)
            Log.d(TAG, "tile z$zoom: $n rows, query %.1f ms, build %.1f ms".format(query, build))
        }

        @Synchronized
        fun drain(what: String): String {
            val line = "$what: $tiles tiles, $rows rows (most in one tile $maxRows), " +
                "query %.0f ms, building features %.0f ms in total, slowest tile %.0f ms".format(queryMs, buildMs, slowest)
            tiles = 0
            rows = 0
            maxRows = 0
            queryMs = 0.0
            buildMs = 0.0
            slowest = 0.0
            return line
        }
    }

    override fun onStart() {
        super.onStart()
        mapView.onStart()
    }

    override fun onResume() {
        super.onResume()
        mapView.onResume()
    }

    override fun onPause() {
        mapView.onPause()
        super.onPause()
    }

    override fun onStop() {
        mapView.onStop()
        super.onStop()
    }

    override fun onLowMemory() {
        super.onLowMemory()
        mapView.onLowMemory()
    }

    override fun onDestroy() {
        mapView.onDestroy()
        GeoCrewNative.closeAll()
        super.onDestroy()
    }

    private companion object {
        const val TAG = "GeoCrewMap"
        const val POINTS = 100_000
        const val EDITS = 10

        /** A plain background, so the spike needs no network or map keys. */
        const val STYLE = """{"version":8,"sources":{},"layers":[{"id":"bg","type":"background","paint":{"background-color":"#f2efe9"}}]}"""
    }
}
