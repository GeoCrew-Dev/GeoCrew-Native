// geocrew_bench: checks and times GDAL, PROJ and GEOS on a device. See README.md.
//
// Usage: geocrew_bench <work dir>
// PROJ_DATA and GDAL_DATA must point at the data directory (proj.db and GDAL's
// data files). Exit status is the number of failed checks.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <gdalalgorithm.h>
#include <geos_c.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>
#include <proj.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPoints = 100000;
// A field area in Hubei, CGCS2000 longitude and latitude.
constexpr double kLon0 = 113.5, kLat0 = 29.5, kSpan = 1.0;
// 花岗岩露头 ("granite outcrop") in UTF-8 and in GBK.
constexpr char kText[] = "\xE8\x8A\xB1\xE5\xB2\x97\xE5\xB2\xA9\xE9\x9C\xB2\xE5\xA4\xB4";
constexpr char kTextGbk[] = "\xBB\xA8\xB8\xDA\xD1\xD2\xC2\xB6\xCD\xB7";

int failures = 0;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void report(const char *what, double ms, const std::string &note = "") {
    std::printf("%-36s %9.1f ms  %s\n", what, ms, note.c_str());
}

void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s (last GDAL error: %s)\n", what, CPLGetLastErrorMsg());
        failures++;
    }
}

OGRSpatialReference srsFromEpsg(int code) {
    OGRSpatialReference srs;
    srs.importFromEPSG(code);
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return srs;
}

void versions() {
    std::printf("GDAL %s, PROJ %s, GEOS %s\n", GDALVersionInfo("RELEASE_NAME"), proj_info().version,
                GEOSversion());
    auto t0 = Clock::now();
    GDALAllRegister();
    report("GDALAllRegister", msSince(t0), CPLSPrintf("%d drivers", GDALGetDriverCount()));
    std::string missing;
    for (const char *name : {"GPKG", "SQLite", "ESRI Shapefile", "GeoJSON", "GeoJSONSeq", "FlatGeobuf",
                             "KML", "GPX", "CSV", "DXF", "XLSX", "ODS", "OpenFileGDB", "MapInfo File",
                             "GTiff", "COG", "MBTiles", "PNG", "JPEG", "WEBP", "VRT", "MEM"}) {
        if (!GDALGetDriverByName(name)) missing += std::string(" ") + name;
    }
    if (!missing.empty()) std::printf("missing drivers:%s\n", missing.c_str());
    check(missing.empty(), "expected drivers present");
}

void gpkg(const std::string &dir, const std::vector<double> &lon, const std::vector<double> &lat) {
    const std::string path = dir + "/bench.gpkg";
    VSIUnlink(path.c_str());
    auto t0 = Clock::now();
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    std::unique_ptr<GDALDataset> ds(driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    OGRSpatialReference srs = srsFromEpsg(4490);
    OGRLayer *layer = ds->CreateLayer("points", &srs, wkbPoint, nullptr);
    OGRFieldDefn name("name", OFTString), elev("elev", OFTReal), note("note", OFTString);
    check(layer->CreateField(&name) == OGRERR_NONE && layer->CreateField(&elev) == OGRERR_NONE &&
              layer->CreateField(&note) == OGRERR_NONE,
          "GPKG create fields");
    ds->StartTransaction();
    int written = 0;
    for (int i = 0; i < kPoints; i++) {
        OGRFeature f(layer->GetLayerDefn());
        f.SetField(0, CPLSPrintf("P%06d", i));
        f.SetField(1, 100.0 + i % 900);
        f.SetField(2, kText);
        OGRPoint p(lon[i], lat[i]);
        f.SetGeometry(&p);
        if (layer->CreateFeature(&f) == OGRERR_NONE) written++;
    }
    ds->CommitTransaction();
    check(written == kPoints, "GPKG write all features");
    ds.reset();
    report("GPKG write 100k points", msSince(t0));

    t0 = Clock::now();
    ds.reset(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
    layer = ds->GetLayer(0);
    int n = 0;
    double sum = 0;
    for (auto &f : *layer) {
        sum += f->GetFieldAsDouble(1);
        n++;
    }
    report("GPKG read 100k points", msSince(t0), CPLSPrintf("%d features, mean elev %.1f", n, n ? sum / n : 0.0));
    check(n == kPoints, "GPKG feature count");

    t0 = Clock::now();
    layer->SetSpatialFilterRect(kLon0 + 0.40, kLat0 + 0.40, kLon0 + 0.50, kLat0 + 0.50);
    n = 0;
    for (auto &f : *layer) {
        (void)f;
        n++;
    }
    report("GPKG bbox query (1% of area)", msSince(t0), CPLSPrintf("%d features", n));
    check(n > kPoints / 200 && n < kPoints / 50, "GPKG spatial filter");
    layer->SetSpatialFilter(nullptr);

    // SQLite math functions back the SQL expressions the app uses.
    OGRLayer *sql = ds->ExecuteSQL("SELECT sqrt(16.0) AS v, count(*) AS n FROM points", nullptr, nullptr);
    OGRFeatureUniquePtr row(sql ? sql->GetNextFeature() : nullptr);
    check(row && row->GetFieldAsDouble(0) == 4.0 && row->GetFieldAsInteger(1) == kPoints, "SQLite math functions");
    row.reset();
    if (sql) ds->ReleaseResultSet(sql);
}

void proj(std::vector<double> &x, std::vector<double> &y) {
    OGRSpatialReference src = srsFromEpsg(4490), dst = srsFromEpsg(4547);  // CGCS2000 / 3-degree GK CM 114E
    auto t0 = Clock::now();
    std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&src, &dst));
    const double setup = msSince(t0);
    check(ct != nullptr, "PROJ transformation 4490 -> 4547");
    if (!ct) return;
    t0 = Clock::now();
    check(ct->Transform(kPoints, x.data(), y.data()), "PROJ transform 100k points");
    report("PROJ 4490->4547 100k points", msSince(t0), CPLSPrintf("setup %.1f ms", setup));
    // On the central meridian at 30N: easting 500000, northing about 3320113.
    double e = 114.0, n = 30.0;
    ct->Transform(1, &e, &n);
    std::printf("  114E 30N -> %.3f %.3f\n", e, n);
    check(std::fabs(e - 500000.0) < 0.001 && std::fabs(n - 3320113.0) < 10.0, "PROJ result plausible");
}

void geos(const std::vector<double> &x, const std::vector<double> &y) {
    auto t0 = Clock::now();
    for (int i = 0; i < 10000; i++) {
        OGRPoint p(x[i], y[i]);
        std::unique_ptr<OGRGeometry> b(p.Buffer(50.0, 8));
        if (!b) {
            check(false, "GEOS buffer");
            return;
        }
    }
    report("GEOS buffer 10k points", msSince(t0));

    // Overlapping 50 m buffers in a 2 km square, merged into one shape.
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> d(0.0, 2000.0);
    OGRMultiPolygon parts;
    for (int i = 0; i < 2000; i++) {
        OGRPoint p(500000.0 + d(rng), 3320000.0 + d(rng));
        std::unique_ptr<OGRGeometry> b(p.Buffer(50.0, 8));
        if (b) parts.addGeometry(b.get());
    }
    t0 = Clock::now();
    std::unique_ptr<OGRGeometry> merged(parts.UnaryUnion());
    report("GEOS union 2k overlapping buffers", msSince(t0),
           merged ? CPLSPrintf("area %.0f m2", OGR_G_Area(OGRGeometry::ToHandle(merged.get()))) : "");
    check(merged != nullptr, "GEOS union");
}

// Chinese shapefiles are usually GBK. GDAL converts through iconv; Android's own
// iconv cannot do GBK, so this checks the bytes on disk, not just the round trip.
void shapefileGbk(const std::string &dir) {
    const std::string path = dir + "/gbk.shp";
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    CPLPushErrorHandler(CPLQuietErrorHandler);
    driver->Delete(path.c_str());
    CPLPopErrorHandler();
    {
        std::unique_ptr<GDALDataset> ds(driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
        CPLStringList options;
        options.SetNameValue("ENCODING", "CP936");
        OGRLayer *layer = ds->CreateLayer("gbk", nullptr, wkbPoint, options.List());
        OGRFieldDefn field("rock", OFTString);
        field.SetWidth(32);
        check(layer->CreateField(&field) == OGRERR_NONE, "shapefile create field");
        OGRFeature f(layer->GetLayerDefn());
        f.SetField(0, kText);
        OGRPoint p(114.0, 30.0);
        f.SetGeometry(&p);
        check(layer->CreateFeature(&f) == OGRERR_NONE, "shapefile write feature");
    }
    std::ifstream dbf(dir + "/gbk.dbf", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(dbf)), std::istreambuf_iterator<char>());
    check(bytes.find(kTextGbk) != std::string::npos, "shapefile written as GBK");

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
    OGRFeatureUniquePtr f(ds ? ds->GetLayer(0)->GetNextFeature() : nullptr);
    check(f && std::strcmp(f->GetFieldAsString(0), kText) == 0, "GBK shapefile read back as UTF-8");
}

void raster(const std::string &dir) {
    const std::string path = dir + "/bench.tif";
    constexpr int kSize = 4096;
    auto t0 = Clock::now();
    CPLStringList options;
    options.SetNameValue("COMPRESS", "ZSTD");
    options.SetNameValue("TILED", "YES");
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    std::unique_ptr<GDALDataset> ds(driver->Create(path.c_str(), kSize, kSize, 1, GDT_Byte, options.List()));
    check(ds != nullptr, "GTiff create with ZSTD");
    if (!ds) return;
    double gt[6] = {500000.0 - kSize * 5.0, 10.0, 0.0, 3320000.0 + kSize * 5.0, 0.0, -10.0};
    ds->SetGeoTransform(gt);
    OGRSpatialReference srs = srsFromEpsg(4547);
    ds->SetSpatialRef(&srs);
    std::vector<GByte> row(kSize);
    GDALRasterBand *band = ds->GetRasterBand(1);
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) row[x] = static_cast<GByte>((x ^ y) & 0xFF);
        if (band->RasterIO(GF_Write, 0, y, kSize, 1, row.data(), kSize, 1, GDT_Byte, 0, 0) != CE_None) break;
    }
    ds.reset();
    report("GTiff write 4096x4096 ZSTD", msSince(t0));

    // The map warps rasters that are not in Web Mercator.
    t0 = Clock::now();
    GDALDatasetH src = GDALOpen(path.c_str(), GA_ReadOnly);
    check(src != nullptr, "GTiff reopen");
    if (!src) return;
    CPLStringList args;
    for (const char *a : {"-of", "MEM", "-t_srs", "EPSG:3857", "-r", "bilinear"}) args.AddString(a);
    GDALWarpAppOptions *wo = GDALWarpAppOptionsNew(args.List(), nullptr);
    int usageError = 0;
    GDALDatasetH out = GDALWarp("", nullptr, 1, &src, wo, &usageError);
    GDALWarpAppOptionsFree(wo);
    report("Warp 4096x4096 to EPSG:3857", msSince(t0),
           out ? CPLSPrintf("%dx%d", GDALGetRasterXSize(out), GDALGetRasterYSize(out)) : "");
    check(out != nullptr, "GDALWarp");
    if (out) GDALClose(out);
    GDALClose(src);
}

// GDAL 3.12's algorithm registry, which the app's processing tools are built on.
void algorithm(const std::string &dir) {
    auto t0 = Clock::now();
    GDALAlgorithmRegistryH registry = GDALGetGlobalAlgorithmRegistry();
    GDALAlgorithmH root = GDALAlgorithmRegistryInstantiateAlg(registry, "gdal");
    GDALAlgorithmRegistryRelease(registry);
    CPLStringList args;
    for (const char *a : {"vector", "info", "--format=json"}) args.AddString(a);
    args.AddString((dir + "/bench.gpkg").c_str());
    std::string out;
    if (root && GDALAlgorithmParseCommandLineArguments(root, args.List())) {
        GDALAlgorithmH alg = GDALAlgorithmGetActualAlgorithm(root);
        if (GDALAlgorithmRun(alg, nullptr, nullptr) && GDALAlgorithmFinalize(alg)) {
            if (GDALAlgorithmArgH arg = GDALAlgorithmGetArg(alg, "output-string")) {
                out = GDALAlgorithmArgGetAsString(arg);
                GDALAlgorithmArgRelease(arg);
            }
        }
        GDALAlgorithmRelease(alg);
    }
    if (root) GDALAlgorithmRelease(root);
    report("gdal vector info (algorithm API)", msSince(t0), CPLSPrintf("%zu bytes of JSON", out.size()));
    check(out.find("\"featureCount\":100000") != std::string::npos ||
              out.find("\"featureCount\": 100000") != std::string::npos,
          "algorithm API output");
}

void peakMemory() {
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (line.rfind("VmHWM:", 0) == 0) std::printf("peak memory %s\n", line.c_str() + 6);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <work dir>\n", argv[0]);
        return 100;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the results if a later step crashes
    const std::string dir = argv[1];
    versions();

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> d(0.0, kSpan);
    std::vector<double> lon(kPoints), lat(kPoints);
    for (int i = 0; i < kPoints; i++) {
        lon[i] = kLon0 + d(rng);
        lat[i] = kLat0 + d(rng);
    }
    gpkg(dir, lon, lat);
    proj(lon, lat);  // lon/lat now hold Gauss-Kruger metres
    geos(lon, lat);
    shapefileGbk(dir);
    raster(dir);
    algorithm(dir);
    peakMemory();
    std::printf("%s: %d failed checks\n", failures ? "FAILED" : "OK", failures);
    return failures;
}
