package org.geocrew.nativelib

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CoordinatesTest {
    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val dataDir = File(context.filesDir, "geocrew-native").apply { mkdirs() }
        for (name in context.assets.list("geocrew-native").orEmpty()) {
            context.assets.open("geocrew-native/$name").use { input ->
                File(dataDir, name).outputStream().use { input.copyTo(it) }
            }
        }
        GeoCrewNative.setup(dataDir.path)
    }

    // EPSG:4547 is CGCS2000 / 3-degree Gauss-Kruger, central meridian 114E.
    @Test
    fun gaussKrugerAtItsCentralMeridianAndBack() {
        val xy = Coordinates.transform("EPSG:4490", "EPSG:4547", doubleArrayOf(114.0, 30.0))
        assertEquals("easting, not northing, comes first", 500_000.0, xy[0], 1e-6)
        assertTrue("about 3320 km from the equator: ${xy[1]}", xy[1] in 3_300_000.0..3_340_000.0)
        val back = Coordinates.transform("EPSG:4547", "EPSG:4490", xy)
        assertEquals(114.0, back[0], 1e-9)
        assertEquals(30.0, back[1], 1e-9)
    }

    // EPSG:4526 is the same zone (38) with the zone number in front of the easting.
    @Test
    fun theZoneNumberCanLeadTheEasting() {
        val xy = Coordinates.transform("EPSG:4490", "EPSG:4526", doubleArrayOf(114.0, 30.0))
        assertEquals(38_500_000.0, xy[0], 1e-6)
    }

    @Test
    fun manyPairsAtOnce() {
        val xy = Coordinates.transform("EPSG:4490", "EPSG:4547", doubleArrayOf(114.0, 30.0, 114.5, 30.5, 113.5, 29.5))
        assertEquals(6, xy.size)
        assertTrue("east of the meridian is east of 500 km", xy[2] > 500_000.0)
        assertTrue("west of it, west", xy[4] < 500_000.0)
        assertEquals(0, Coordinates.transform("EPSG:4490", "EPSG:4547", DoubleArray(0)).size)
    }

    @Test
    fun badInputIsRefused() {
        assertThrows(IllegalArgumentException::class.java) { Coordinates.transform("EPSG:999999", "EPSG:4490", doubleArrayOf(0.0, 0.0)) }
        assertThrows(IllegalArgumentException::class.java) { Coordinates.transform("EPSG:4490", "EPSG:4547", doubleArrayOf(114.0)) }
        assertThrows(IllegalArgumentException::class.java) { Coordinates.describe("not a crs") }
    }

    @Test
    fun aCrsDescribesItself() {
        val gk = JSONObject(Coordinates.describe("EPSG:4547"))
        assertEquals("EPSG:4547", gk.getString("id"))
        assertTrue(gk.getBoolean("projected"))
        assertFalse(gk.getBoolean("geographic"))
        assertEquals("metre", gk.getString("unit"))
        assertTrue(gk.getString("name"), gk.getString("name").contains("CGCS2000"))
        val geographic = JSONObject(Coordinates.describe("EPSG:4490"))
        assertTrue(geographic.getBoolean("geographic"))
        assertEquals("degree", geographic.getString("unit"))
    }
}
