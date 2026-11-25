// JNI entry points for the app. For now: library versions, setup, and running a
// GDAL algorithm by its command line, enough to prove the library loads and to
// make the linker keep everything the app will use (drivers and algorithms).
#include <jni.h>

#include <string>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdalalgorithm.h>
#include <geos_c.h>
#include <ogr_srs_api.h>
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
