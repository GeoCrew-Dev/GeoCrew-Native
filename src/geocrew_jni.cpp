// JNI entry points for the app. For now: library versions, setup, and running a
// GDAL algorithm by its command line, enough to prove the library loads and to
// make the linker keep everything the app will use (drivers and algorithms).
#include <jni.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <gdalalgorithm.h>
#include <geos_c.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>
#include <ogrsf_frmts.h>
#include <proj.h>

#include "jni_util.h"

using namespace geocrew;

extern "C" JNIEXPORT jstring JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_versions(JNIEnv *env, jclass) {
    std::string v = std::string("GDAL ") + GDALVersionInfo("RELEASE_NAME") + ", PROJ " +
                    proj_info().version + ", GEOS " + GEOSversion();
    return fromUtf8(env, v);
}

// dataDir holds proj.db and GDAL's data files, unpacked from the AAR's assets.
extern "C" JNIEXPORT void JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_setup(JNIEnv *env, jclass, jstring dataDir) {
    if (!dataDir) {
        throwIllegalArgument(env, "dataDir is null");
        return;
    }
    std::string dir = toUtf8(env, dataDir);
    const char *const paths[] = {dir.c_str(), nullptr};
    OSRSetPROJSearchPaths(paths);
    CPLSetConfigOption("GDAL_DATA", dir.c_str());
    GDALAllRegister();
}

// Runs "gdal <args...>" and returns its text output. Throws with GDAL's last
// error message when the arguments are wrong or the algorithm fails.
extern "C" JNIEXPORT jstring JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_run(JNIEnv *env, jclass, jobjectArray args) {
    if (!args) {
        throwIllegalArgument(env, "args is null");
        return nullptr;
    }
    CPLStringList argv;
    for (jsize i = 0, n = env->GetArrayLength(args); i < n; i++) {
        auto arg = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        if (!arg) {
            throwIllegalArgument(env, "args contains null");
            return nullptr;
        }
        argv.AddString(toUtf8(env, arg).c_str());
        env->DeleteLocalRef(arg);
    }
    CPLErrorReset();
    GDALAlgorithmRegistryH registry = GDALGetGlobalAlgorithmRegistry();
    GDALAlgorithmH root = GDALAlgorithmRegistryInstantiateAlg(registry, "gdal");
    GDALAlgorithmRegistryRelease(registry);
    if (!root) {
        throwGdalError(env, "GDAL algorithms are not available");
        return nullptr;
    }
    jstring result = nullptr;
    if (!GDALAlgorithmParseCommandLineArguments(root, argv.List())) {
        throwGdalError(env, "invalid arguments");
    } else {
        GDALAlgorithmH alg = GDALAlgorithmGetActualAlgorithm(root);
        if (GDALAlgorithmRun(alg, nullptr, nullptr) && GDALAlgorithmFinalize(alg)) {
            std::string out;
            if (GDALAlgorithmArgH arg = GDALAlgorithmGetArg(alg, "output-string")) {
                out = GDALAlgorithmArgGetAsString(arg);
                GDALAlgorithmArgRelease(arg);
            }
            result = fromUtf8(env, out);
        } else {
            throwGdalError(env, "algorithm failed");
        }
        GDALAlgorithmRelease(alg);
    }
    GDALAlgorithmRelease(root);
    return result;
}

// ---------------------------------------------------------------------------
// Map spike (M1): what CustomGeometrySource needs. Datasets stay open between
// calls, because MapLibre asks for every tile separately, from several threads;
// one lock serialises them.
// ---------------------------------------------------------------------------

namespace {

std::mutex gDatasetsLock;
std::map<std::string, std::unique_ptr<GDALDataset>> gDatasets;
bool gClosed = false;  // set by closeAll, so late tile queries do not reopen files

// The open dataset for path, opened for update on first use. Call with gDatasetsLock held.
GDALDataset *dataset(const std::string &path) {
    if (gClosed) return nullptr;
    auto &ds = gDatasets[path];
    if (!ds) ds.reset(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    return ds.get();
}

}  // namespace

// Runs sql, which must return a point geometry and an integer class as its
// first field, and returns {double[] x0,y0,x1,y1,..., int[] classes}.
// Throws with GDAL's message when the query fails.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_queryPoints(JNIEnv *env, jclass, jstring path, jstring sql) {
    if (!path || !sql) {
        throwIllegalArgument(env, "path or sql is null");
        return nullptr;
    }
    const std::string p = toUtf8(env, path), q = toUtf8(env, sql);
    std::vector<double> xy;
    std::vector<jint> classes;
    {
        std::lock_guard<std::mutex> lock(gDatasetsLock);
        CPLErrorReset();
        GDALDataset *ds = dataset(p);
        OGRLayer *rs = ds ? ds->ExecuteSQL(q.c_str(), nullptr, nullptr) : nullptr;
        if (!rs) {
            throwGdalError(env, "query failed");
            return nullptr;
        }
        for (auto &f : *rs) {
            const OGRGeometry *g = f->GetGeometryRef();
            if (!g || wkbFlatten(g->getGeometryType()) != wkbPoint) continue;
            xy.push_back(g->toPoint()->getX());
            xy.push_back(g->toPoint()->getY());
            classes.push_back(f->GetFieldCount() > 0 ? f->GetFieldAsInteger(0) : 0);
        }
        ds->ReleaseResultSet(rs);
    }
    jdoubleArray coords = env->NewDoubleArray(static_cast<jsize>(xy.size()));
    env->SetDoubleArrayRegion(coords, 0, static_cast<jsize>(xy.size()), xy.data());
    jintArray cls = env->NewIntArray(static_cast<jsize>(classes.size()));
    env->SetIntArrayRegion(cls, 0, static_cast<jsize>(classes.size()), classes.data());
    jobjectArray out = env->NewObjectArray(2, env->FindClass("java/lang/Object"), nullptr);
    env->SetObjectArrayElement(out, 0, coords);
    env->SetObjectArrayElement(out, 1, cls);
    return out;
}

// Moves point feature fid of layer to (x, y) through OGR, the way the app's
// editor will write, so the GeoPackage's index triggers run. Throws on failure.
extern "C" JNIEXPORT void JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_movePoint(JNIEnv *env, jclass, jstring path, jstring layerName, jlong fid,
                                                   jdouble x, jdouble y) {
    if (!path || !layerName) {
        throwIllegalArgument(env, "path or layer is null");
        return;
    }
    const std::string p = toUtf8(env, path), l = toUtf8(env, layerName);
    std::lock_guard<std::mutex> lock(gDatasetsLock);
    CPLErrorReset();
    GDALDataset *ds = dataset(p);
    OGRLayer *layer = ds ? ds->GetLayerByName(l.c_str()) : nullptr;
    OGRFeatureUniquePtr f(layer ? layer->GetFeature(fid) : nullptr);
    if (!f) {
        throwGdalError(env, "feature not found");
        return;
    }
    OGRPoint point(x, y);
    f->SetGeometry(&point);
    if (layer->SetFeature(f.get()) != OGRERR_NONE) throwGdalError(env, "cannot save the feature");
}

// Closes every dataset opened by queryPoints or movePoint; later calls fail.
extern "C" JNIEXPORT void JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_closeAll(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lock(gDatasetsLock);
    gClosed = true;
    gDatasets.clear();
}

// ---------------------------------------------------------------------------
// Raster spike (M1): rasters in any CRS drawn by MapLibre. Each Web Mercator
// tile is warped from the source on request and encoded as PNG.
// ---------------------------------------------------------------------------

namespace {

constexpr double kMercatorOrigin = 20037508.342789244;
constexpr int kTileSize = 256;

struct Raster {
    std::unique_ptr<GDALDataset> ds;
    OGREnvelope footprint;  // the raster's extent in EPSG:3857
};

std::mutex gRastersLock;
std::map<std::string, Raster> gRasters;

// The open raster for path with its Web Mercator footprint, or nullptr (and no
// cache entry) when it cannot be used. Call with gRastersLock held.
Raster *openRaster(const std::string &path, Raster &r);

Raster *raster(const std::string &path) {
    auto it = gRasters.find(path);
    if (it != gRasters.end()) return &it->second;
    Raster r;
    if (!openRaster(path, r)) return nullptr;
    return &gRasters.emplace(path, std::move(r)).first->second;
}

Raster *openRaster(const std::string &path, Raster &r) {
    r.ds.reset(GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    if (!r.ds) return nullptr;
    double gt[6];
    const OGRSpatialReference *srs = r.ds->GetSpatialRef();
    if (r.ds->GetGeoTransform(gt) != CE_None || !srs) return nullptr;
    OGRSpatialReference mercator;
    mercator.importFromEPSG(3857);
    mercator.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(srs, &mercator));
    if (!ct) return nullptr;
    // Edges sampled every 1/16 of the side, since straight edges curve after reprojection.
    const double w = r.ds->GetRasterXSize(), h = r.ds->GetRasterYSize();
    for (int i = 0; i <= 16; i++) {
        const double pixels[4][2] = {{w * i / 16, 0}, {w * i / 16, h}, {0, h * i / 16}, {w, h * i / 16}};
        for (const auto &px : pixels) {
            double x = gt[0] + px[0] * gt[1] + px[1] * gt[2];
            double y = gt[3] + px[0] * gt[4] + px[1] * gt[5];
            if (ct->Transform(1, &x, &y)) r.footprint.Merge(x, y);
        }
    }
    return &r;
}

}  // namespace

// Renders Web Mercator tile z/x/y (XYZ scheme, 256 px) of the raster at path,
// warping from the raster's own CRS, and returns it as PNG with transparency
// where the raster does not reach; an empty array for a tile outside it.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_renderTile(JNIEnv *env, jclass, jstring path, jint z, jint x, jint y) {
    if (!path) {
        throwIllegalArgument(env, "path is null");
        return nullptr;
    }
    const std::string p = toUtf8(env, path);
    const double span = 2 * kMercatorOrigin / static_cast<double>(1LL << z);
    const double minX = -kMercatorOrigin + x * span, maxY = kMercatorOrigin - y * span;
    std::vector<GByte> png;
    {
        std::lock_guard<std::mutex> lock(gRastersLock);
        CPLErrorReset();
        Raster *r = raster(p);
        if (!r) {
            throwGdalError(env, "cannot open the raster");
            return nullptr;
        }
        OGREnvelope tile;
        tile.MinX = minX;
        tile.MaxX = minX + span;
        tile.MinY = maxY - span;
        tile.MaxY = maxY;
        if (tile.Intersects(r->footprint)) {
            const int count = r->ds->GetRasterCount();
            const bool srcAlpha = r->ds->GetRasterBand(count)->GetColorInterpretation() == GCI_AlphaBand;
            const int bands = count - (srcAlpha ? 1 : 0) >= 3 ? 3 : 1;
            GDALDriver *mem = GetGDALDriverManager()->GetDriverByName("MEM");
            std::unique_ptr<GDALDataset> dst(mem->Create("", kTileSize, kTileSize, bands + 1, GDT_Byte, nullptr));
            double gt[6] = {minX, span / kTileSize, 0, maxY, 0, -span / kTileSize};
            dst->SetGeoTransform(gt);
            OGRSpatialReference mercator;
            mercator.importFromEPSG(3857);
            dst->SetSpatialRef(&mercator);
            CPLStringList args;
            for (const char *a : {"-r", "bilinear", "-dstalpha"}) args.AddString(a);
            if (srcAlpha) args.AddString("-srcalpha");  // carry the source's own transparency
            for (int b = 1; b <= bands; b++) {
                args.AddString("-b");
                args.AddString(CPLSPrintf("%d", b));
            }
            GDALWarpAppOptions *options = GDALWarpAppOptionsNew(args.List(), nullptr);
            GDALDatasetH src = GDALDataset::ToHandle(r->ds.get());
            int usageError = 0;
            GDALDatasetH warped = GDALWarp(nullptr, GDALDataset::ToHandle(dst.get()), 1, &src, options, &usageError);
            GDALWarpAppOptionsFree(options);
            if (!warped) {
                throwGdalError(env, "warp failed");
                return nullptr;
            }
            const char *name = "/vsimem/geocrew_tile.png";
            GDALDriver *pngDriver = GetGDALDriverManager()->GetDriverByName("PNG");
            std::unique_ptr<GDALDataset> out(pngDriver->CreateCopy(name, dst.get(), FALSE, nullptr, nullptr, nullptr));
            out.reset();
            vsi_l_offset size = 0;
            GByte *data = VSIGetMemFileBuffer(name, &size, FALSE);
            if (data) png.assign(data, data + size);
            VSIUnlink(name);
            if (png.empty()) {
                throwGdalError(env, "PNG encoding failed");
                return nullptr;
            }
        }
    }
    jbyteArray out = env->NewByteArray(static_cast<jsize>(png.size()));
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(png.size()), reinterpret_cast<const jbyte *>(png.data()));
    return out;
}

// Writes a 40 km x 40 km RGB GeoTIFF in CGCS2000 / 3-degree Gauss-Kruger CM 114E
// (EPSG:4547) around 114E 30N, 10 m pixels: colour gradients with a black line
// every kilometre, so misplaced or distorted tiles are easy to see.
extern "C" JNIEXPORT void JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_makeTestRaster(JNIEnv *env, jclass, jstring path) {
    if (!path) {
        throwIllegalArgument(env, "path is null");
        return;
    }
    const std::string p = toUtf8(env, path);
    constexpr int kSize = 4000;
    CPLStringList options;
    options.SetNameValue("COMPRESS", "DEFLATE");
    options.SetNameValue("TILED", "YES");
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    std::unique_ptr<GDALDataset> ds(driver->Create(p.c_str(), kSize, kSize, 3, GDT_Byte, options.List()));
    if (!ds) {
        throwGdalError(env, "cannot create the raster");
        return;
    }
    double gt[6] = {480000.0, 10.0, 0, 3340000.0, 0, -10.0};
    ds->SetGeoTransform(gt);
    OGRSpatialReference srs;
    srs.importFromEPSG(4547);
    ds->SetSpatialRef(&srs);
    std::vector<GByte> row(kSize);
    for (int b = 1; b <= 3; b++) {
        GDALRasterBand *band = ds->GetRasterBand(b);
        for (int j = 0; j < kSize; j++) {
            for (int i = 0; i < kSize; i++) {
                const bool line = i % 100 == 0 || j % 100 == 0;
                const int value = b == 1 ? i * 255 / kSize : b == 2 ? j * 255 / kSize : 160;
                row[i] = static_cast<GByte>(line ? 0 : value);
            }
            if (band->RasterIO(GF_Write, 0, j, kSize, 1, row.data(), kSize, 1, GDT_Byte, 0, 0) != CE_None) {
                throwGdalError(env, "cannot write the raster");
                return;
            }
        }
    }
}
