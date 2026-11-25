package org.geocrew.nativelib

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.BeforeClass
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GeoCrewNativeTest {
    companion object {
        private lateinit var dir: File

        @BeforeClass
        @JvmStatic
        fun setUp() {
            val context = InstrumentationRegistry.getInstrumentation().targetContext
            dir = context.filesDir
            val dataDir = File(dir, "geocrew-native").apply { mkdirs() }
            for (name in context.assets.list("geocrew-native").orEmpty()) {
                context.assets.open("geocrew-native/$name").use { input ->
                    File(dataDir, name).outputStream().use { input.copyTo(it) }
                }
            }
            GeoCrewNative.setup(dataDir.path)
        }
    }

    @Test
    fun versions() {
        val v = GeoCrewNative.versions()
        assertTrue(v, v.startsWith("GDAL 3.12"))
    }

    @Test
    fun reprojectThroughAlgorithmPipeline() {
        val input = File(dir, "outcrop.geojson")
        input.writeText(
            """{"type":"FeatureCollection","features":[{"type":"Feature",""" +
                """"properties":{"rock":"花岗岩露头"},"geometry":{"type":"Point","coordinates":[114.0,30.0]}}]}""",
        )
        val output = File(dir, "outcrop.gpkg").apply { delete() }
        val converted = GeoCrewNative.run(
            arrayOf("vector", "pipeline", "read", input.path, "!", "reproject", "--dst-crs", "EPSG:4547", "!", "write", output.path),
        )
        assertNotNull("pipeline failed", converted)
        val info = GeoCrewNative.run(arrayOf("vector", "info", "--format=json", "--features", output.path))
        assertNotNull("info failed", info)
        assertTrue(info!!, info.contains("4547"))
        assertTrue(info, info.contains("花岗岩露头"))
        // 114E 30N on the 114E central meridian: northing about 3320113.4 m.
        assertTrue(info, info.contains("3320113"))
    }
}
