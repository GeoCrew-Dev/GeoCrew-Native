// Coordinates between CRSs for the app (ADR 0015 in the main repository):
// positions from GNSS and the map, in CGCS2000 longitude and latitude, go to a
// layer's CRS and back. x is always the longitude or easting and y the latitude
// or northing, whatever axis order the CRS's own definition gives.
#include <jni.h>

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <cpl_json.h>
#include <ogr_spatialref.h>

#include "jni_util.h"

using namespace geocrew;

namespace {

std::mutex gLock;

// A transformation is costly to set up and cheap to run: one per pair of CRSs,
// kept; past kMaxKept pairs (users typing their own WKT, say) the cache starts over.
constexpr size_t kMaxKept = 64;
std::map<std::pair<std::string, std::string>, std::unique_ptr<OGRCoordinateTransformation>> gTransforms;

// The CRS named by text as GDAL reads user input (EPSG:4549, WKT, PROJ strings;
// no files or URLs), x first; false with an exception pending when GDAL does not know it.
bool readSrs(JNIEnv *env, const std::string &text, OGRSpatialReference &srs) {
    if (srs.SetFromUserInput(text.c_str(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE) {
        throwIllegalArgument(env, ("unknown CRS " + text).c_str());
        return false;
    }
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return true;
}

}  // namespace

extern "C" JNIEXPORT jdoubleArray JNICALL
Java_org_geocrew_nativelib_Coordinates_transform(JNIEnv *env, jclass, jstring from, jstring to, jdoubleArray xy) {
    if (!from || !to || !xy) {
        throwIllegalArgument(env, "a CRS or the coordinates are null");
        return nullptr;
    }
    const jsize n = env->GetArrayLength(xy);
    if (n % 2 != 0) {
        throwIllegalArgument(env, "coordinates come in x, y pairs");
        return nullptr;
    }
    const auto key = std::make_pair(toUtf8(env, from), toUtf8(env, to));
    std::lock_guard<std::mutex> lock(gLock);
    auto it = gTransforms.find(key);
    if (it == gTransforms.end()) {
        OGRSpatialReference a, b;
        if (!readSrs(env, key.first, a) || !readSrs(env, key.second, b)) return nullptr;
        if (gTransforms.size() >= kMaxKept) gTransforms.clear();
        std::unique_ptr<OGRCoordinateTransformation> t(OGRCreateCoordinateTransformation(&a, &b));
        if (!t) {
            throwIllegalArgument(env, ("no transformation from " + key.first + " to " + key.second).c_str());
            return nullptr;
        }
        it = gTransforms.emplace(key, std::move(t)).first;
    }
    const size_t count = static_cast<size_t>(n / 2);
    std::vector<double> x(count), y(count);
    std::vector<int> ok(count, FALSE);
    jdouble *in = env->GetDoubleArrayElements(xy, nullptr);
    if (!in) return nullptr;  // OutOfMemoryError pending
    for (size_t i = 0; i < count; i++) {
        x[i] = in[2 * i];
        y[i] = in[2 * i + 1];
    }
    env->ReleaseDoubleArrayElements(xy, in, JNI_ABORT);
    if (count > 0) it->second->Transform(count, x.data(), y.data(), nullptr, ok.data());
    // A pair PROJ cannot transform (outside the CRS's area of use, say) comes back as NaN.
    std::vector<jdouble> out(2 * count);
    for (size_t i = 0; i < count; i++) {
        const bool good = ok[i] && std::isfinite(x[i]) && std::isfinite(y[i]);
        out[2 * i] = good ? x[i] : NAN;
        out[2 * i + 1] = good ? y[i] : NAN;
    }
    jdoubleArray result = env->NewDoubleArray(n);
    if (result) env->SetDoubleArrayRegion(result, 0, n, out.data());
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_geocrew_nativelib_Coordinates_describe(JNIEnv *env, jclass, jstring text) {
    if (!text) {
        throwIllegalArgument(env, "the CRS is null");
        return nullptr;
    }
    OGRSpatialReference srs;
    if (!readSrs(env, toUtf8(env, text), srs)) return nullptr;
    CPLJSONObject o;
    o.Add("name", srs.GetName() ? srs.GetName() : "");
    const char *authority = srs.GetAuthorityName(nullptr);
    const char *code = srs.GetAuthorityCode(nullptr);
    if (authority && code) o.Add("id", std::string(authority) + ":" + code);
    o.Add("geographic", srs.IsGeographic() != 0);
    o.Add("projected", srs.IsProjected() != 0);
    const char *unit = nullptr;
    if (srs.IsProjected()) {
        srs.GetLinearUnits(&unit);
    } else {
        srs.GetAngularUnits(&unit);
    }
    o.Add("unit", unit ? unit : "");
    return fromUtf8(env, o.Format(CPLJSONObject::PrettyFormat::Plain));
}
