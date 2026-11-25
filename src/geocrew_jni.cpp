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
#include <gdalalgorithm.h>
#include <geos_c.h>
#include <ogr_srs_api.h>
#include <ogrsf_frmts.h>
#include <proj.h>

namespace {

// JNI's own UTF-8 is "modified UTF-8", which differs from GDAL's standard UTF-8
// for characters outside the BMP (rare CJK characters, emoji). Convert through
// UTF-16 instead. Unpaired surrogates become U+FFFD.
std::string toUtf8(JNIEnv *env, jstring s) {
    const jsize n = env->GetStringLength(s);
    const jchar *u = env->GetStringChars(s, nullptr);
    std::string out;
    out.reserve(n);
    for (jsize i = 0; i < n; i++) {
        char32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    env->ReleaseStringChars(s, u);
    return out;
}

// Standard UTF-8 to a Java string. Invalid bytes become U+FFFD.
jstring fromUtf8(JNIEnv *env, const std::string &s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        int extra = b < 0x80 ? 0 : (b >> 5) == 0x6 ? 1 : (b >> 4) == 0xE ? 2 : (b >> 3) == 0x1E ? 3 : -1;
        char32_t c = extra == 0 ? b : extra == 1 ? (b & 0x1F) : extra == 2 ? (b & 0x0F) : (b & 0x07);
        bool ok = extra >= 0;
        for (int k = 1; ok && k <= extra; k++) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                ok = false;
            } else {
                c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            }
        }
        if (!ok || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            out += u'�';
            i++;
            continue;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
        i += extra + 1;
    }
    return env->NewString(reinterpret_cast<const jchar *>(out.data()), static_cast<jsize>(out.size()));
}

void throwIllegalArgument(JNIEnv *env, const char *message) {
    if (jclass cls = env->FindClass("java/lang/IllegalArgumentException")) env->ThrowNew(cls, message);
}

void throwGdalError(JNIEnv *env, const char *what) {
    const char *detail = CPLGetLastErrorMsg();
    std::string message = std::string(what) + (detail && *detail ? std::string(": ") + detail : std::string());
    if (jclass cls = env->FindClass("java/lang/RuntimeException")) env->ThrowNew(cls, message.c_str());
}

}  // namespace

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

// The open dataset for path, opened for update on first use. Call with gDatasetsLock held.
GDALDataset *dataset(const std::string &path) {
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

// Closes every dataset opened by queryPoints or movePoint.
extern "C" JNIEXPORT void JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_closeAll(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lock(gDatasetsLock);
    gDatasets.clear();
}
