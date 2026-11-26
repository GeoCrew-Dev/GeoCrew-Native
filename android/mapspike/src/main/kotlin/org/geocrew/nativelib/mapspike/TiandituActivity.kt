package org.geocrew.nativelib.mapspike

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.widget.FrameLayout
import android.widget.TextView
import java.io.File
import java.io.IOException
import kotlin.math.PI
import kotlin.math.atan
import kotlin.math.floor
import kotlin.math.min
import kotlin.math.sinh
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
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

/**
 * M1 spike: 天地图 (Tianditu) base maps with the owner's key.
 *
 * - mode "w" (default): Tianditu's Web Mercator tiles straight into MapLibre: the vector map
 *   (vec_w + cva_w), or with `--es layer img` the imagery (img_w + cia_w).
 * - mode "c": Tianditu's lat/lon tiles (vec_c, TILEMATRIXSET=c) warped to Web Mercator on
 *   request: for each tile MapLibre asks for, the lat/lon tiles that cover it are fetched,
 *   stitched, described by a VRT in EPSG:4326 and warped by GeoCrewNative.renderTile.
 *
 * The key is read from files/tianditu-key.txt in the app's private storage (pushed with
 * `adb shell run-as`), never from the source or the APK, and is never logged.
 * Fetched lat/lon tiles are kept in cache/tdt-c so a rerun does not use the daily quota again.
 * Logs under the tag "GeoCrewTdt".
 *
 * MapLibre's logo and attribution button are hidden: the license (BSD-2-Clause) does not ask for
 * them, and the real app shows its own data attribution (Tianditu's 审图号).
 *
 * Start: adb shell am start -n org.geocrew.nativelib.mapspike/.TiandituActivity --es mode c
 */
class TiandituActivity : Activity() {
    private lateinit var mapView: MapView
    private lateinit var status: TextView
    private lateinit var map: MapLibreMap
    private lateinit var key: String
    private lateinit var tileDir: File
    private val report = StringBuilder()
    private val lock = Any()
    private val renderLock = Any()
    private val fetchMs = mutableListOf<Double>()
    private val warpMs = mutableListOf<Double>()
    private var cached = 0
    private var failed = 0
    private val net = OkHttpClient()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MapLibre.getInstance(this)
        // MapLibre logs the URL of cancelled and failed requests, and Tianditu's carry the key.
        org.maplibre.android.module.http.HttpRequestUtil.setLogEnabled(false)
        val mode = intent.getStringExtra("mode") ?: "w"
        mapView = MapView(this)
        status = TextView(this).apply {
            setBackgroundColor(0xCCFFFFFF.toInt())
            textSize = 11f
        }
        setContentView(FrameLayout(this).apply {
            addView(mapView)
            addView(status, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))
        })
        mapView.onCreate(savedInstanceState)
        val keyFile = File(filesDir, "tianditu-key.txt")
        key = if (keyFile.exists()) keyFile.readText().trim().removePrefix("﻿") else ""
        if (!Regex("[0-9a-fA-F]{32}").matches(key)) {
            log("no usable key in files/tianditu-key.txt (expected 32 hex digits)")
            return
        }
        tileDir = File(cacheDir, "tdt-c").apply { mkdirs() }
        if (mode == "c") installWarpingClient()
        Thread {
            if (mode == "c") prepareNative()
            runOnUiThread { mapView.getMapAsync { start(it, mode) } }
        }.start()
    }

    private fun prepareNative() {
        val dataDir = File(filesDir, "geocrew-native").apply { mkdirs() }
        for (name in assets.list("geocrew-native").orEmpty()) {
            val target = File(dataDir, name)
            if (!target.exists()) assets.open("geocrew-native/$name").use { input -> target.outputStream().use { input.copyTo(it) } }
        }
        GeoCrewNative.setup(dataDir.path)
    }

    private fun installWarpingClient() {
        org.maplibre.android.module.http.HttpRequestUtil.setOkHttpClient(OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            if (request.url.host != TILE_HOST) return@addInterceptor chain.proceed(request)
            val tile = runCatching {
                request.url.pathSegments.let { listOf(it[0], it[1], it[2].substringBefore('.')) }.map { it.toInt() }
            }.getOrNull() ?: return@addInterceptor failure(request, "bad tile path")
            val (z, x, y) = tile
            val png = try {
                warpedTile(z, x, y)
            } catch (e: IOException) {
                return@addInterceptor failure(request, "tile $z/$x/$y: ${e.message}")
            } catch (e: RuntimeException) {
                return@addInterceptor failure(request, "tile $z/$x/$y: ${e.message}")
            }
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1)
                .code(if (png.isEmpty()) 204 else 200).message("OK")
                .body(png.toResponseBody("image/png".toMediaType()))
                .build()
        }.build())
    }

    /** Web Mercator tile z/x/y built from the Tianditu lat/lon tiles at the same level. */
    private fun warpedTile(z: Int, x: Int, y: Int): ByteArray {
        val n = (1 shl z).toDouble()
        val west = x / n * 360 - 180
        val east = (x + 1) / n * 360 - 180
        val north = Math.toDegrees(atan(sinh(PI * (1 - 2 * y / n))))
        val south = Math.toDegrees(atan(sinh(PI * (1 - 2 * (y + 1) / n))))
        // TILEMATRIXSET=c: level L has 2^L x 2^(L-1) tiles of 360 / 2^L degrees, from (-180, 90).
        val level = min(z.coerceAtLeast(1), 18)
        val span = 360.0 / (1 shl level)
        val col0 = floor((west + 180) / span).toInt()
        val col1 = floor((east + 180 - 1e-9) / span).toInt()
        val row0 = floor((90 - north) / span).toInt()
        val row1 = floor((90 - south - 1e-9) / span).toInt()
        val cols = col1 - col0 + 1
        val rows = row1 - row0 + 1
        val mosaic = Bitmap.createBitmap(cols * 256, rows * 256, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(mosaic)
        for (r in row0..row1) for (c in col0..col1) {
            val bytes = latLonTile(level, r, c)
            val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: throw IOException("tile $level/$r/$c is not an image")
            canvas.drawBitmap(bmp, ((c - col0) * 256).toFloat(), ((r - row0) * 256).toFloat(), null)
        }
        val base = File(tileDir, "m_${z}_${x}_$y")
        val png = File("$base.png")
        writeAtomically(png) { file -> file.outputStream().use { mosaic.compress(Bitmap.CompressFormat.PNG, 100, it) } }
        val vrt = File("$base.vrt")
        writeAtomically(vrt) { it.writeText(vrtXml(png.name, cols * 256, rows * 256, -180 + col0 * span, 90 - row0 * span, span / 256)) }
        return synchronized(renderLock) {
            val t0 = SystemClock.elapsedRealtimeNanos()
            val out = GeoCrewNative.renderTile(vrt.path, z, x, y)
            synchronized(lock) { warpMs += (SystemClock.elapsedRealtimeNanos() - t0) / 1e6 }
            out
        }
    }

    /** One Tianditu lat/lon tile, from the cache or the network. */
    private fun latLonTile(level: Int, row: Int, col: Int): ByteArray {
        val file = File(tileDir, "vec_c_${level}_${row}_$col.png")
        if (file.exists() && file.length() > 0) {
            synchronized(lock) { cached++ }
            return file.readBytes()
        }
        val url = "https://t${(row + col) % 8}.tianditu.gov.cn/vec_c/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0" +
            "&LAYER=vec&STYLE=default&TILEMATRIXSET=c&FORMAT=tiles&TILEMATRIX=$level&TILEROW=$row&TILECOL=$col&tk=$key"
        val t0 = SystemClock.elapsedRealtimeNanos()
        net.newCall(Request.Builder().url(url).build()).execute().use { response ->
            val body = response.body?.bytes() ?: ByteArray(0)
            val type = response.header("Content-Type").orEmpty()
            if (!response.isSuccessful || !type.startsWith("image/")) {
                synchronized(lock) { failed++ }
                // Only the status and type: the URL carries the key.
                throw IOException("Tianditu answered ${response.code} $type for $level/$row/$col")
            }
            synchronized(lock) {
                fetchMs += (SystemClock.elapsedRealtimeNanos() - t0) / 1e6
            }
            writeAtomically(file) { it.writeBytes(body) }
            return body
        }
    }

    /**
     * Writes through a temporary file and renames it into place, so OkHttp's other threads,
     * which may need the same file at the same moment, never read a half-written one.
     */
    private fun writeAtomically(target: File, write: (File) -> Unit) {
        val tmp = File.createTempFile(target.name, ".tmp", target.parentFile)
        try {
            write(tmp)
            if (!tmp.renameTo(target)) throw IOException("cannot rename ${tmp.name}")
        } finally {
            tmp.delete()
        }
    }

    private fun vrtXml(png: String, w: Int, h: Int, west: Double, north: Double, res: Double): String {
        val bands = listOf("Red", "Green", "Blue", "Alpha").mapIndexed { i, interp ->
            """<VRTRasterBand dataType="Byte" band="${i + 1}"><ColorInterp>$interp</ColorInterp>""" +
                """<SimpleSource><SourceFilename relativeToVRT="1">$png</SourceFilename><SourceBand>${i + 1}</SourceBand>""" +
                """<SrcRect xOff="0" yOff="0" xSize="$w" ySize="$h"/><DstRect xOff="0" yOff="0" xSize="$w" ySize="$h"/></SimpleSource></VRTRasterBand>"""
        }.joinToString("")
        return """<VRTDataset rasterXSize="$w" rasterYSize="$h"><SRS>EPSG:4326</SRS>""" +
            "<GeoTransform>$west, $res, 0, $north, 0, ${-res}</GeoTransform>$bands</VRTDataset>"
    }

    private fun failure(request: Request, message: String): Response {
        Log.w(TAG, message)
        return Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(500).message("tile failed")
            .body(ByteArray(0).toResponseBody(null)).build()
    }

    private fun start(map: MapLibreMap, mode: String) {
        this.map = map
        map.uiSettings.isLogoEnabled = false
        map.uiSettings.isAttributionEnabled = false
        val style = when {
            mode == "c" -> STYLE_C
            intent.getStringExtra("layer") == "img" -> styleW("img", "cia")
            else -> styleW("vec", "cva")
        }.replace("KEY", key)
        map.setStyle(Style.Builder().fromJson(style)) {
            map.cameraPosition = CameraPosition.Builder().target(LatLng(39.91, 116.40)).zoom(10.0).build()
            val t0 = SystemClock.elapsedRealtime()
            whenFullyRendered {
                log("mode $mode: first full render at zoom 10 over Beijing: ${SystemClock.elapsedRealtime() - t0} ms")
                log(drain("tiles for the first render"))
                zoomTest(mode)
            }
        }
    }

    /** Zooms 10 → 14 → 12 → 16, two seconds each, then holds for screenshots. */
    private fun zoomTest(mode: String) {
        val zooms = listOf(14.0, 12.0, 16.0)
        var frames = 0
        val listener = MapView.OnDidFinishRenderingFrameListener { _, _, _ -> frames++ }
        mapView.addOnDidFinishRenderingFrameListener(listener)
        val t0 = SystemClock.elapsedRealtime()
        fun step(i: Int) {
            if (i == zooms.size) {
                val seconds = (SystemClock.elapsedRealtime() - t0) / 1000.0
                mapView.removeOnDidFinishRenderingFrameListener(listener)
                log("mode $mode: zooming for %.1f s: %d frames, %.1f fps".format(seconds, frames, frames / seconds))
                mapView.postDelayed({
                    log(drain("tiles while zooming"))
                    log("done")
                }, 3000)
                return
            }
            map.animateCamera(CameraUpdateFactory.zoomTo(zooms[i]), 2000, object : MapLibreMap.CancelableCallback {
                override fun onFinish() = step(i + 1)
                override fun onCancel() = step(i + 1)
            })
        }
        step(0)
    }

    private fun drain(what: String): String = synchronized(lock) {
        fun stats(v: List<Double>) = if (v.isEmpty()) "none" else v.sorted().let {
            "%d, median %.1f ms, max %.1f ms".format(it.size, it[it.size / 2], it.last())
        }
        val line = "$what: lat/lon tiles fetched ${stats(fetchMs)}; from cache $cached; failed $failed; warped ${stats(warpMs)}"
        fetchMs.clear()
        warpMs.clear()
        cached = 0
        failed = 0
        line
    }

    private fun whenFullyRendered(then: () -> Unit) {
        var called = false
        lateinit var listener: MapView.OnDidFinishRenderingMapListener
        val timeout = Runnable {
            if (!called) {
                called = true
                mapView.removeOnDidFinishRenderingMapListener(listener)
                log("no fully rendered map within 15 s")
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
        mapView.postDelayed(timeout, 15000)
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
        const val TAG = "GeoCrewTdt"
        const val TILE_HOST = "geocrew.tiles"

        private const val W = "https://t0.tianditu.gov.cn/@L@_w/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=@L@&STYLE=default&TILEMATRIXSET=w&FORMAT=tiles&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}&tk=KEY"

        /** A Tianditu Web Mercator base layer (vec, img) and its labels (cva, cia), straight from Tianditu. */
        fun styleW(base: String, labels: String) =
            """{"version":8,"sources":{"base":{"type":"raster","tiles":["${W.replace("@L@", base)}"],"tileSize":256,"maxzoom":18},""" +
                """"labels":{"type":"raster","tiles":["${W.replace("@L@", labels)}"],"tileSize":256,"maxzoom":18}},""" +
                """"layers":[{"id":"bg","type":"background","paint":{"background-color":"#f2efe9"}},{"id":"base","type":"raster","source":"base"},{"id":"labels","type":"raster","source":"labels"}]}"""

        /** The lat/lon vector map, warped in the app. */
        const val STYLE_C = """{"version":8,"sources":{"c":{"type":"raster","tiles":["https://$TILE_HOST/{z}/{x}/{y}.png"],"tileSize":256,"maxzoom":18}},"layers":[{"id":"bg","type":"background","paint":{"background-color":"#f2efe9"}},{"id":"c","type":"raster","source":"c"}]}"""
    }
}
