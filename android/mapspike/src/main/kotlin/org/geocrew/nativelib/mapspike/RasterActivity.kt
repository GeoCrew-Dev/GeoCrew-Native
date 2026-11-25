package org.geocrew.nativelib.mapspike

import android.app.Activity
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.widget.FrameLayout
import android.widget.TextView
import java.io.File
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.geocrew.nativelib.GeoCrewNative
import org.maplibre.android.MapLibre
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.module.http.HttpRequestUtil

/**
 * M1 spike: a raster in a non-Web-Mercator CRS (a Gauss-Kruger GeoTIFF) shown
 * by MapLibre. Tile requests to https://geocrew.tiles/z/x/y.png never reach the
 * network: an OkHttp interceptor answers them with tiles GDAL warps on the spot.
 * Logs under the tag "GeoCrewRaster": the first full render, the frame rate while
 * zooming, and what each tile cost.
 */
class RasterActivity : Activity() {
    private lateinit var mapView: MapView
    private lateinit var status: TextView
    private lateinit var tiff: File
    private lateinit var map: MapLibreMap
    private val report = StringBuilder()
    private val lock = Any()
    private val renderLock = Any()
    private val tileMs = mutableListOf<Double>()
    private var emptyTiles = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MapLibre.getInstance(this)
        HttpRequestUtil.setOkHttpClient(OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            if (request.url.host != TILE_HOST) return@addInterceptor chain.proceed(request)
            // Anything thrown here other than an IOException would kill OkHttp's thread, so answer 500 instead.
            val tile = runCatching {
                request.url.pathSegments.let { listOf(it[0], it[1], it[2].substringBefore('.')) }.map { it.toInt() }
            }.getOrNull()
            if (tile == null) return@addInterceptor failure(request, "bad tile path ${request.url}")
            val (z, x, y) = tile
            // renderTile serialises on one lock inside; taking it here first keeps
            // the time spent waiting for other tiles out of the measurement.
            val png = synchronized(renderLock) {
                val t0 = SystemClock.elapsedRealtimeNanos()
                val bytes = try {
                    GeoCrewNative.renderTile(tiff.path, z, x, y)
                } catch (e: RuntimeException) {
                    return@addInterceptor failure(request, "tile $z/$x/$y: ${e.message}")
                }
                val ms = (SystemClock.elapsedRealtimeNanos() - t0) / 1e6
                synchronized(lock) { if (bytes.isEmpty()) emptyTiles++ else tileMs += ms }
                bytes
            }
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1)
                .code(if (png.isEmpty()) 204 else 200).message("OK")
                .body(png.toResponseBody("image/png".toMediaType()))
                .build()
        }.build())
        mapView = MapView(this)
        status = TextView(this).apply {
            setBackgroundColor(0xCCFFFFFF.toInt())
            textSize = 11f
        }
        setContentView(FrameLayout(this).apply {
            addView(mapView)
            // Only as tall as its text, so it does not wash out the whole map.
            addView(status, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        })
        mapView.onCreate(savedInstanceState)
        Thread {
            prepare()
            runOnUiThread { mapView.getMapAsync(::start) }
        }.start()
    }

    private fun failure(request: okhttp3.Request, message: String): Response {
        Log.w(TAG, message)
        return Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(500).message(message)
            .body(ByteArray(0).toResponseBody(null)).build()
    }

    private fun prepare() {
        val dataDir = File(filesDir, "geocrew-native").apply { mkdirs() }
        for (name in assets.list("geocrew-native").orEmpty()) {
            val target = File(dataDir, name)
            if (!target.exists()) assets.open("geocrew-native/$name").use { input -> target.outputStream().use { input.copyTo(it) } }
        }
        GeoCrewNative.setup(dataDir.path)
        tiff = File(filesDir, "gauss-kruger.tif")
        if (!tiff.exists()) {
            val t0 = SystemClock.elapsedRealtime()
            GeoCrewNative.makeTestRaster(tiff.path)
            log("wrote the 4000 x 4000 test GeoTIFF (EPSG:4547) in ${SystemClock.elapsedRealtime() - t0} ms")
        }
    }

    private fun start(map: MapLibreMap) {
        this.map = map
        map.setStyle(Style.Builder().fromJson(STYLE)) {
            map.cameraPosition = CameraPosition.Builder().target(LatLng(30.0, 114.0)).zoom(10.5).build()
            val t0 = SystemClock.elapsedRealtime()
            whenFullyRendered {
                log("first full render at zoom 10.5 (whole raster): ${SystemClock.elapsedRealtime() - t0} ms")
                log(drainTiles("tiles for the first render"))
                zoomTest()
            }
        }
    }

    /** Zooms 10.5 → 14 → 10.5 → 16 → 12, two seconds each, and reports the frame rate. */
    private fun zoomTest() {
        val zooms = listOf(14.0, 10.5, 16.0, 12.0)
        var frames = 0
        val listener = MapView.OnDidFinishRenderingFrameListener { _, _, _ -> frames++ }
        mapView.addOnDidFinishRenderingFrameListener(listener)
        val t0 = SystemClock.elapsedRealtime()
        fun step(i: Int) {
            if (i == zooms.size) {
                val seconds = (SystemClock.elapsedRealtime() - t0) / 1000.0
                mapView.removeOnDidFinishRenderingFrameListener(listener)
                log("zooming for %.1f s: %d frames, %.1f fps".format(seconds, frames, frames / seconds))
                mapView.postDelayed({
                    log(drainTiles("tiles while zooming"))
                    log("done")
                }, 2000)
                return
            }
            map.animateCamera(CameraUpdateFactory.zoomTo(zooms[i]), 2000, object : MapLibreMap.CancelableCallback {
                override fun onFinish() = step(i + 1)
                override fun onCancel() = step(i + 1)
            })
        }
        step(0)
    }

    private fun drainTiles(what: String): String = synchronized(lock) {
        val sorted = tileMs.sorted()
        val line = if (sorted.isEmpty()) {
            "$what: none rendered, $emptyTiles outside the raster"
        } else {
            "$what: %d rendered, median %.1f ms, max %.1f ms, %.0f ms in total; %d outside the raster"
                .format(sorted.size, sorted[sorted.size / 2], sorted.last(), sorted.sum(), emptyTiles)
        }
        tileMs.clear()
        emptyTiles = 0
        line
    }

    /** Calls [then] once, after the next fully rendered map, or after 10 seconds if none comes. */
    private fun whenFullyRendered(then: () -> Unit) {
        var called = false
        lateinit var listener: MapView.OnDidFinishRenderingMapListener
        val timeout = Runnable {
            if (!called) {
                called = true
                mapView.removeOnDidFinishRenderingMapListener(listener)
                log("no fully rendered map within 10 s")
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
        mapView.postDelayed(timeout, 10000)
        map.triggerRepaint()
    }

    private fun log(line: String) {
        Log.i(TAG, line)
        runOnUiThread {
            report.appendLine(line)
            status.text = report
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
        super.onDestroy()
    }

    private companion object {
        const val TAG = "GeoCrewRaster"
        const val TILE_HOST = "geocrew.tiles"

        /** A plain background and the warped raster on top; no network needed. */
        const val STYLE = """{"version":8,"sources":{"gk":{"type":"raster","tiles":["https://$TILE_HOST/{z}/{x}/{y}.png"],"tileSize":256}},"layers":[{"id":"bg","type":"background","paint":{"background-color":"#f2efe9"}},{"id":"gk","type":"raster","source":"gk"}]}"""
    }
}
