package org.geocrew.nativelib

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.json.JSONArray
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class VectorDatasetTest {
    private lateinit var path: File
    private lateinit var ds: VectorDataset

    private val fields = """[{"name": "rock", "type": "string"}, {"name": "dip", "type": "real"},
        {"name": "n", "type": "integer64"}, {"name": "ok", "type": "boolean"}, {"name": "day", "type": "date"},
        {"name": "at", "type": "datetime"}]"""

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
        path = File(context.filesDir, "vector-test.gpkg")
        for (suffix in listOf("", "-wal", "-shm")) File(path.path + suffix).delete()
        ds = VectorDataset.create(path.path)
        ds.createLayer("points", "POINT Z", "EPSG:4547", fields)
    }

    @After
    fun tearDown() = ds.close()

    @Test
    fun describesTheLayer() {
        assertEquals(listOf("points"), ds.layers)
        val info = JSONObject(ds.layerInfo("points"))
        assertEquals("POINT Z", info.getString("geometryType"))
        assertEquals("EPSG:4547", info.getString("srs"))
        assertEquals(0, info.getInt("count"))
        val types = info.getJSONArray("fields").let { a -> (0 until a.length()).map { a.getJSONObject(it).getString("type") } }
        assertEquals(listOf("string", "real", "integer64", "boolean", "date", "datetime"), types)
        val caps = info.getJSONObject("capabilities")
        assertTrue(caps.getBoolean("randomWrite") && caps.getBoolean("deleteFeature") && caps.getBoolean("transactions"))
        assertEquals("wal", JSONArray(ds.sql("PRAGMA journal_mode")).getJSONObject(0).getJSONObject("properties").getString("journal_mode"))
    }

    // Coordinates in metres with millimetres, and reals, must read back exactly.
    @Test
    fun valuesSurviveTheRoundTrip() {
        val fid = ds.add(
            "points",
            """{"geometry": {"type": "Point", "coordinates": [500123.4567891234, 3320113.0123456789, 87.1]},
               "properties": {"rock": "花岗岩 \"𠮷\"\n", "dip": 0.1, "n": 9007199254740993, "ok": true,
                              "day": "2026-11-26", "at": "2026-11-26T08:30:15Z"}}""",
        )
        val f = JSONObject(ds.feature("points", fid)!!)
        assertEquals(fid, f.getLong("fid"))
        val xyz = f.getJSONObject("geometry").getJSONArray("coordinates")
        assertEquals(500123.4567891234, xyz.getDouble(0), 0.0)
        assertEquals(3320113.0123456789, xyz.getDouble(1), 0.0)
        assertEquals(87.1, xyz.getDouble(2), 0.0)
        val p = f.getJSONObject("properties")
        assertEquals("花岗岩 \"𠮷\"\n", p.getString("rock"))
        assertEquals(0.1, p.getDouble("dip"), 0.0)
        assertTrue(ds.feature("points", fid)!!.contains("\"dip\":0.1,"))
        assertEquals(9007199254740993L, p.getLong("n"))
        assertTrue(p.getBoolean("ok"))
        assertEquals("2026-11-26", p.getString("day"))
        assertTrue(p.getString("at"), p.getString("at").startsWith("2026-11-26T08:30:15"))
    }

    @Test
    fun updateChangesOnlyWhatItNames() {
        val fid = ds.add("points", """{"geometry": {"type": "Point", "coordinates": [1, 2, 3]}, "properties": {"rock": "a", "dip": 30}}""")
        ds.update("points", fid, """{"properties": {"dip": 45.5, "n": null}}""")
        var p = JSONObject(ds.feature("points", fid)!!)
        assertEquals("a", p.getJSONObject("properties").getString("rock"))
        assertEquals(45.5, p.getJSONObject("properties").getDouble("dip"), 0.0)
        assertEquals(1.0, p.getJSONObject("geometry").getJSONArray("coordinates").getDouble(0), 0.0)
        ds.update("points", fid, """{"geometry": null, "properties": {"rock": null}}""")
        p = JSONObject(ds.feature("points", fid)!!)
        assertTrue(p.isNull("geometry"))
        assertTrue(p.getJSONObject("properties").isNull("rock"))
        ds.delete("points", fid)
        assertNull(ds.feature("points", fid))
        assertThrows(IllegalArgumentException::class.java) { ds.delete("points", fid) }
        assertThrows(IllegalArgumentException::class.java) { ds.update("points", fid, "{}") }
    }

    @Test
    fun filtersByAttributeAndBox() {
        for ((i, rock) in listOf("a", "b", "a").withIndex()) {
            ds.add("points", """{"geometry": {"type": "Point", "coordinates": [${i * 10}, 0, 0]}, "properties": {"rock": "$rock"}}""")
        }
        assertEquals(3, JSONArray(ds.features("points")).length())
        assertEquals(2, JSONArray(ds.features("points", where = "rock = 'a'")).length())
        assertEquals(1, JSONArray(ds.features("points", where = "rock = 'a'", bbox = doubleArrayOf(15.0, -1.0, 25.0, 1.0))).length())
        assertEquals("filters do not stick", 3, JSONArray(ds.features("points")).length())
    }

    @Test
    fun rollbackTakesBackEverything() {
        ds.add("points", """{"properties": {"rock": "kept"}}""")
        ds.begin()
        ds.add("points", """{"properties": {"rock": "dropped"}}""")
        ds.rollback()
        assertEquals(1, JSONArray(ds.features("points")).length())
        ds.begin()
        ds.add("points", """{"properties": {"rock": "saved"}}""")
        ds.commit()
        ds.close()
        VectorDataset.open(path.path).use {
            assertEquals("rows", 2, JSONArray(it.features("points")).length())
            assertEquals("sql", "2", JSONArray(it.sql("SELECT count(*) AS n FROM points")).getJSONObject(0).getJSONObject("properties").get("n").toString())
            assertEquals("count", 2, JSONObject(it.layerInfo("points")).getInt("count"))
        }
    }

    @Test
    fun badInputIsRefused() {
        assertThrows(IllegalArgumentException::class.java) { ds.add("points", """{"properties": {"colour": "red"}}""") }
        assertThrows(IllegalArgumentException::class.java) { ds.add("points", """{"geometry": {"type": "Blob"}}""") }
        assertThrows(IllegalArgumentException::class.java) { ds.add("points", "not json") }
        for (bad in listOf(""""dip": "45a"""", """"n": 1.5""", """"rock": true""", """"day": 3""", """"ok": 1e10""")) {
            assertThrows(bad, IllegalArgumentException::class.java) { ds.add("points", """{"properties": {$bad}}""") }
        }
        assertThrows(IllegalArgumentException::class.java) { ds.add("nowhere", "{}") }
        assertThrows(IllegalArgumentException::class.java) { ds.createLayer("x", "POINT", null, """[{"name": "a", "type": "blob"}]""") }
        assertThrows(IllegalArgumentException::class.java) { VectorDataset.create(path.path) }
        assertThrows(RuntimeException::class.java) { ds.sql("SELECT * FROM nowhere") }
        assertEquals("nothing was written", 0, JSONArray(ds.features("points")).length())
        ds.close()
        assertThrows(IllegalStateException::class.java) { ds.layers }
        ds.close() // twice is fine
    }

    @Test
    fun readOnlyCannotWrite() {
        ds.close()
        VectorDataset.open(path.path).use { ro ->
            assertFalse(JSONObject(ro.layerInfo("points")).getJSONObject("capabilities").getBoolean("randomWrite"))
            assertThrows(RuntimeException::class.java) { ro.add("points", "{}") }
        }
    }
}
