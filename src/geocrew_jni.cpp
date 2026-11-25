// JNI entry points for the app. For now: library versions, setup, and running a
// GDAL algorithm by its command line, enough to prove the library loads and to
// make the linker keep everything the app will use (drivers and algorithms).
#include <jni.h>

#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdalalgorithm.h>
#include <geos_c.h>
#include <ogr_srs_api.h>
#include <proj.h>

namespace {

std::string toString(JNIEnv *env, jstring s) {
    const char *chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_versions(JNIEnv *env, jclass) {
    std::string v = std::string("GDAL ") + GDALVersionInfo("RELEASE_NAME") + ", PROJ " +
                    proj_info().version + ", GEOS " + GEOSversion();
    return env->NewStringUTF(v.c_str());
}

// dataDir holds proj.db and GDAL's data files, unpacked from the AAR's assets.
extern "C" JNIEXPORT void JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_setup(JNIEnv *env, jclass, jstring dataDir) {
    std::string dir = toString(env, dataDir);
    const char *const paths[] = {dir.c_str(), nullptr};
    OSRSetPROJSearchPaths(paths);
    CPLSetConfigOption("GDAL_DATA", dir.c_str());
    GDALAllRegister();
}

// Runs "gdal <args...>" and returns its text output, or null on failure.
extern "C" JNIEXPORT jstring JNICALL
Java_org_geocrew_nativelib_GeoCrewNative_run(JNIEnv *env, jclass, jobjectArray args) {
    CPLStringList argv;
    for (jsize i = 0, n = env->GetArrayLength(args); i < n; i++) {
        auto arg = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        argv.AddString(toString(env, arg).c_str());
        env->DeleteLocalRef(arg);
    }
    GDALAlgorithmRegistryH registry = GDALGetGlobalAlgorithmRegistry();
    GDALAlgorithmH root = GDALAlgorithmRegistryInstantiateAlg(registry, "gdal");
    GDALAlgorithmRegistryRelease(registry);
    if (!root) return nullptr;
    jstring result = nullptr;
    if (GDALAlgorithmParseCommandLineArguments(root, argv.List())) {
        GDALAlgorithmH alg = GDALAlgorithmGetActualAlgorithm(root);
        if (GDALAlgorithmRun(alg, nullptr, nullptr) && GDALAlgorithmFinalize(alg)) {
            std::string out;
            if (GDALAlgorithmArgH arg = GDALAlgorithmGetArg(alg, "output-string")) {
                out = GDALAlgorithmArgGetAsString(arg);
                GDALAlgorithmArgRelease(arg);
            }
            result = env->NewStringUTF(out.c_str());
        }
        GDALAlgorithmRelease(alg);
    }
    GDALAlgorithmRelease(root);
    return result;
}
