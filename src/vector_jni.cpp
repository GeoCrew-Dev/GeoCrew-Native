// Vector data for the app (ADR 0004 and 0005 in the main repository): datasets
// the app keeps open and reads and writes feature by feature. Features cross JNI
// as JSON, {"fid": 1, "geometry": <GeoJSON geometry or null>, "properties": {...}},
// in the layer's own CRS. Numbers are written so that they read back exactly.
#include <jni.h>

#include <climits>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <cpl_json.h>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>

#include "jni_util.h"

using namespace geocrew;

namespace {

// GDAL datasets are not thread-safe, so one call runs at a time.
std::mutex gLock;
std::map<jlong, std::unique_ptr<GDALDataset>> gOpen;
jlong gNext = 1;

void throwIllegalState(JNIEnv *env, const char *message) {
    if (jclass cls = env->FindClass("java/lang/IllegalStateException")) env->ThrowNew(cls, message);
}

// The open dataset behind handle h, or nullptr with an exception pending.
GDALDataset *dataset(JNIEnv *env, jlong h) {
    auto it = gOpen.find(h);
    if (it == gOpen.end()) {
        throwIllegalState(env, "the dataset is closed");
        return nullptr;
    }
    return it->second.get();
}

// The layer called name in dataset h, or nullptr with an exception pending.
OGRLayer *layer(JNIEnv *env, jlong h, jstring name) {
    GDALDataset *ds = dataset(env, h);
    if (!ds) return nullptr;
    if (!name) {
        throwIllegalArgument(env, "layer is null");
        return nullptr;
    }
    const std::string n = toUtf8(env, name);
    OGRLayer *l = ds->GetLayerByName(n.c_str());
    if (!l) throwIllegalArgument(env, ("there is no layer " + n).c_str());
    return l;
}

// SQLite settings for files opened to write (ADR 0004): every save is fsynced,
// and the app's own projects use WAL. GDAL reads them when it opens the file.
struct SqliteSettings {
    explicit SqliteSettings(bool wal) {
        CPLSetThreadLocalConfigOption("OGR_SQLITE_SYNCHRONOUS", "FULL");
        if (wal) CPLSetThreadLocalConfigOption("OGR_SQLITE_JOURNAL", "WAL");
    }
    ~SqliteSettings() {
        CPLSetThreadLocalConfigOption("OGR_SQLITE_SYNCHRONOUS", nullptr);
        CPLSetThreadLocalConfigOption("OGR_SQLITE_JOURNAL", nullptr);
    }
};

jlong keep(GDALDataset *ds) {
    const jlong h = gNext++;
    gOpen[h].reset(ds);
    return h;
}

// ----- JSON out -----

void quote(std::string &out, const char *s) {
    out += '"';
    for (auto p = reinterpret_cast<const unsigned char *>(s); *p; p++) {
        switch (*p) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (*p < 0x20) {
                    out += CPLSPrintf("\\u%04x", *p);
                } else {
                    out += static_cast<char>(*p);
                }
        }
    }
    out += '"';
}

// The shortest of 15 to 17 significant digits that reads back as the same double.
void number(std::string &out, double d) {
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    char buf[40];
    for (int digits = 15; digits <= 17; digits++) {
        CPLsnprintf(buf, sizeof buf, "%.*g", digits, d);
        if (CPLStrtod(buf, nullptr) == d) break;
    }
    out += buf;
}

void featureJson(std::string &out, OGRFeature *f) {
    out += "{\"fid\":";
    out += std::to_string(f->GetFID());
    out += ",\"geometry\":";
    if (const OGRGeometry *g = f->GetGeometryRef()) {
        const char *const options[] = {"SIGNIFICANT_FIGURES=17", nullptr};
        char *json = g->exportToJson(options);
        out += json ? json : "null";
        CPLFree(json);
    } else {
        out += "null";
    }
    out += ",\"properties\":{";
    const OGRFeatureDefn *defn = f->GetDefnRef();
    for (int i = 0; i < defn->GetFieldCount(); i++) {
        if (i) out += ',';
        const OGRFieldDefn *field = defn->GetFieldDefn(i);
        quote(out, field->GetNameRef());
        out += ':';
        if (!f->IsFieldSetAndNotNull(i)) {
            out += "null";
            continue;
        }
        switch (field->GetType()) {
            case OFTInteger:
                if (field->GetSubType() == OFSTBoolean) {
                    out += f->GetFieldAsInteger(i) ? "true" : "false";
                } else {
                    out += std::to_string(f->GetFieldAsInteger(i));
                }
                break;
            case OFTInteger64: out += std::to_string(f->GetFieldAsInteger64(i)); break;
            case OFTReal: number(out, f->GetFieldAsDouble(i)); break;
            case OFTDate: {
                int y, m, d, hh, mm, tz;
                float ss;
                f->GetFieldAsDateTime(i, &y, &m, &d, &hh, &mm, &ss, &tz);
                quote(out, CPLSPrintf("%04d-%02d-%02d", y, m, d));
                break;
            }
            case OFTDateTime: {
                quote(out, f->GetFieldAsISO8601DateTime(i, nullptr));
                break;
            }
            default: quote(out, f->GetFieldAsString(i));
        }
    }
    out += "}}";
}

// ----- JSON in -----

// Sets field i of f to a JSON value of a fitting type: OGR itself would turn
// "45a" into 0 for a number field, or 1.5 into 1 for an integer one. Returns
// why not, starting with a space, or "".
std::string setValue(OGRFeature *f, int i, const CPLJSONObject &v) {
    const OGRFieldType type = f->GetFieldDefnRef(i)->GetType();
    const bool integer = type == OFTInteger || type == OFTInteger64, numeric = integer || type == OFTReal;
    switch (v.GetType()) {
        case CPLJSONObject::Type::Null: f->SetFieldNull(i); return "";
        case CPLJSONObject::Type::Boolean:
            if (type != OFTInteger) return " does not take true or false";
            f->SetField(i, v.ToBool() ? 1 : 0);
            return "";
        case CPLJSONObject::Type::Integer:
        case CPLJSONObject::Type::Long: {
            if (!numeric && type != OFTString) return " does not take a number";
            const GIntBig n = v.ToLong();
            if (type == OFTInteger && (n < INT_MIN || n > INT_MAX)) return " is a 32-bit integer";
            f->SetField(i, n);
            return "";
        }
        case CPLJSONObject::Type::Double: {
            if (!numeric && type != OFTString) return " does not take a number";
            const double d = v.ToDouble();
            if (integer) {
                const double limit = type == OFTInteger ? 2147483648.0 : 9223372036854775808.0;
                if (d != std::floor(d) || d < -limit || d >= limit) return " takes whole numbers in its range";
                f->SetField(i, static_cast<GIntBig>(d));
            } else {
                f->SetField(i, d);
            }
            return "";
        }
        case CPLJSONObject::Type::String:
            if (numeric) return " takes a number, not a string";
            f->SetField(i, v.ToString().c_str());
            return "";
        default: return " must be a string, number, boolean or null";
    }
}

// Sets f's geometry and properties from {"geometry": ..., "properties": {...}};
// what the JSON leaves out stays as it is. Returns why not, or "".
std::string fill(OGRFeature *f, const std::string &text) {
    CPLJSONDocument doc;
    if (!doc.LoadMemory(text)) return "the feature is not valid JSON";
    const CPLJSONObject root = doc.GetRoot();
    if (root.GetType() != CPLJSONObject::Type::Object) return "the feature must be a JSON object";
    for (const CPLJSONObject &part : root.GetChildren()) {
        const std::string key = part.GetName();
        if (key == "geometry") {
            if (part.GetType() == CPLJSONObject::Type::Null) {
                f->SetGeometryDirectly(nullptr);
            } else {
                OGRGeometry *g = OGRGeometryFactory::createFromGeoJson(part);
                if (!g) return "the geometry is not valid GeoJSON";
                f->SetGeometryDirectly(g);
            }
        } else if (key == "properties") {
            if (part.GetType() != CPLJSONObject::Type::Object) return "properties must be an object";
            for (const CPLJSONObject &p : part.GetChildren()) {
                const std::string name = p.GetName();
                const int i = f->GetFieldIndex(name.c_str());
                if (i < 0) return "there is no field " + name;
                const std::string why = setValue(f, i, p);
                if (!why.empty()) return "field " + name + why;
            }
        } else if (key != "fid" && key != "type") {
            return "unknown member " + key;
        }
    }
    return "";
}

bool fieldType(const std::string &name, OGRFieldType &type, OGRFieldSubType &sub) {
    sub = OFSTNone;
    if (name == "integer") {
        type = OFTInteger;
    } else if (name == "integer64") {
        type = OFTInteger64;
    } else if (name == "real") {
        type = OFTReal;
    } else if (name == "string") {
        type = OFTString;
    } else if (name == "date") {
        type = OFTDate;
    } else if (name == "datetime") {
        type = OFTDateTime;
    } else if (name == "boolean") {
        type = OFTInteger;
        sub = OFSTBoolean;
    } else {
        return false;
    }
    return true;
}

const char *fieldTypeName(const OGRFieldDefn *f) {
    switch (f->GetType()) {
        case OFTInteger: return f->GetSubType() == OFSTBoolean ? "boolean" : "integer";
        case OFTInteger64: return "integer64";
        case OFTReal: return "real";
        case OFTString: return "string";
        case OFTDate: return "date";
        case OFTDateTime: return "datetime";
        default: return "other";
    }
}

std::string orEmpty(JNIEnv *env, jstring s) { return s ? toUtf8(env, s) : std::string(); }

}  // namespace

#define VECTOR(name) Java_org_geocrew_nativelib_VectorNative_##name

extern "C" JNIEXPORT jlong JNICALL VECTOR(open)(JNIEnv *env, jclass, jstring path, jboolean update, jboolean wal) {
    if (!path) {
        throwIllegalArgument(env, "path is null");
        return 0;
    }
    const std::string p = toUtf8(env, path);
    std::lock_guard<std::mutex> lock(gLock);
    CPLErrorReset();
    SqliteSettings settings(update && wal);
    GDALDataset *ds = GDALDataset::Open(p.c_str(), GDAL_OF_VECTOR | (update ? GDAL_OF_UPDATE : GDAL_OF_READONLY));
    if (!ds) {
        throwGdalError(env, ("cannot open " + p).c_str());
        return 0;
    }
    return keep(ds);
}

extern "C" JNIEXPORT jlong JNICALL VECTOR(create)(JNIEnv *env, jclass, jstring path, jstring driver, jboolean wal) {
    if (!path || !driver) {
        throwIllegalArgument(env, "path or driver is null");
        return 0;
    }
    const std::string p = toUtf8(env, path), d = toUtf8(env, driver);
    std::lock_guard<std::mutex> lock(gLock);
    CPLErrorReset();
    VSIStatBufL stat;
    if (VSIStatL(p.c_str(), &stat) == 0) {
        throwIllegalArgument(env, (p + " already exists").c_str());
        return 0;
    }
    GDALDriver *drv = GetGDALDriverManager()->GetDriverByName(d.c_str());
    if (!drv) {
        throwIllegalArgument(env, ("there is no driver " + d).c_str());
        return 0;
    }
    SqliteSettings settings(wal);
    GDALDataset *ds = drv->Create(p.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!ds) {
        throwGdalError(env, ("cannot create " + p).c_str());
        return 0;
    }
    return keep(ds);
}

extern "C" JNIEXPORT void JNICALL VECTOR(close)(JNIEnv *, jclass, jlong h) {
    std::lock_guard<std::mutex> lock(gLock);
    gOpen.erase(h);
}

extern "C" JNIEXPORT jobjectArray JNICALL VECTOR(layerNames)(JNIEnv *env, jclass, jlong h) {
    std::lock_guard<std::mutex> lock(gLock);
    GDALDataset *ds = dataset(env, h);
    if (!ds) return nullptr;
    const int n = ds->GetLayerCount();
    jobjectArray out = env->NewObjectArray(n, env->FindClass("java/lang/String"), nullptr);
    for (int i = 0; i < n; i++) {
        jstring name = fromUtf8(env, ds->GetLayer(i)->GetName());
        env->SetObjectArrayElement(out, i, name);
        env->DeleteLocalRef(name);
    }
    return out;
}

extern "C" JNIEXPORT jstring JNICALL VECTOR(layerInfo)(JNIEnv *env, jclass, jlong h, jstring name) {
    std::lock_guard<std::mutex> lock(gLock);
    OGRLayer *l = layer(env, h, name);
    if (!l) return nullptr;
    GDALDataset *ds = dataset(env, h);
    std::string out = "{\"name\":";
    quote(out, l->GetName());
    out += ",\"geometryType\":";
    quote(out, OGRToOGCGeomType(l->GetGeomType(), false, true, true));
    out += ",\"srs\":";
    if (const OGRSpatialReference *srs = l->GetSpatialRef()) {
        const char *auth = srs->GetAuthorityName(nullptr), *code = srs->GetAuthorityCode(nullptr);
        if (auth && code) {
            quote(out, CPLSPrintf("%s:%s", auth, code));
        } else {
            char *wkt = nullptr;
            const char *const options[] = {"FORMAT=WKT2", nullptr};
            srs->exportToWkt(&wkt, options);
            quote(out, wkt ? wkt : "");
            CPLFree(wkt);
        }
    } else {
        out += "null";
    }
    out += ",\"count\":";
    out += std::to_string(l->GetFeatureCount());
    out += ",\"fields\":[";
    OGRFeatureDefn *defn = l->GetLayerDefn();
    for (int i = 0; i < defn->GetFieldCount(); i++) {
        if (i) out += ',';
        const OGRFieldDefn *f = defn->GetFieldDefn(i);
        out += "{\"name\":";
        quote(out, f->GetNameRef());
        out += ",\"type\":";
        quote(out, fieldTypeName(f));
        out += '}';
    }
    out += "],\"capabilities\":{";
    out += std::string("\"randomWrite\":") + (l->TestCapability(OLCRandomWrite) ? "true" : "false");
    out += std::string(",\"deleteFeature\":") + (l->TestCapability(OLCDeleteFeature) ? "true" : "false");
    out += std::string(",\"createField\":") + (l->TestCapability(OLCCreateField) ? "true" : "false");
    out += std::string(",\"transactions\":") + (ds->TestCapability(ODsCTransactions) ? "true" : "false");
    out += "}}";
    return fromUtf8(env, out);
}

// fields is a JSON array of {"name": ..., "type": integer|integer64|real|string|date|datetime|boolean}.
extern "C" JNIEXPORT void JNICALL VECTOR(createLayer)(JNIEnv *env, jclass, jlong h, jstring name, jstring geometryType,
                                                       jstring srs, jstring fields) {
    if (!name || !geometryType || !fields) {
        throwIllegalArgument(env, "name, geometryType or fields is null");
        return;
    }
    const std::string n = toUtf8(env, name), g = toUtf8(env, geometryType), s = orEmpty(env, srs),
                      fs = toUtf8(env, fields);
    std::lock_guard<std::mutex> lock(gLock);
    GDALDataset *ds = dataset(env, h);
    if (!ds) return;
    CPLErrorReset();
    const OGRwkbGeometryType type = OGRFromOGCGeomType(g.c_str());
    if (type == wkbUnknown && !EQUAL(g.c_str(), "GEOMETRY")) {
        throwIllegalArgument(env, ("unknown geometry type " + g).c_str());
        return;
    }
    OGRSpatialReference ref;
    ref.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (!s.empty() && ref.SetFromUserInput(s.c_str(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS) !=
                          OGRERR_NONE) {
        throwIllegalArgument(env, ("unknown CRS " + s).c_str());
        return;
    }
    CPLJSONDocument doc;
    if (!doc.LoadMemory(fs) || doc.GetRoot().GetType() != CPLJSONObject::Type::Array) {
        throwIllegalArgument(env, "fields must be a JSON array");
        return;
    }
    std::vector<OGRFieldDefn> defs;
    for (const CPLJSONObject &f : doc.GetRoot().ToArray()) {
        OGRFieldType t;
        OGRFieldSubType sub;
        const std::string fname = f.GetString("name"), ftype = f.GetString("type");
        if (fname.empty() || !fieldType(ftype, t, sub)) {
            throwIllegalArgument(env, ("bad field " + f.Format(CPLJSONObject::PrettyFormat::Plain)).c_str());
            return;
        }
        defs.emplace_back(fname.c_str(), t);
        defs.back().SetSubType(sub);
    }
    OGRLayer *l = ds->CreateLayer(n.c_str(), s.empty() ? nullptr : &ref, type, nullptr);
    if (!l) {
        throwGdalError(env, ("cannot create layer " + n).c_str());
        return;
    }
    for (OGRFieldDefn &d : defs) {
        if (l->CreateField(&d) != OGRERR_NONE) {
            throwGdalError(env, (std::string("cannot create field ") + d.GetNameRef()).c_str());
            for (int i = 0; i < ds->GetLayerCount(); i++) {
                if (ds->GetLayer(i) == l) ds->DeleteLayer(i);  // not half a layer
            }
            return;
        }
    }
    // A GeoPackage creates the table on the first write; do it now, or a transaction
    // that is rolled back would take the table with it while GDAL still sees the layer.
    if (l->SyncToDisk() != OGRERR_NONE) throwGdalError(env, ("cannot create layer " + n).c_str());
}

// Every feature of the layer, or those matching where (an OGR SQL condition)
// and bbox (minX, minY, maxX, maxY in the layer's CRS), as a JSON array.
extern "C" JNIEXPORT jstring JNICALL VECTOR(features)(JNIEnv *env, jclass, jlong h, jstring name, jstring where,
                                                       jdoubleArray bbox) {
    const std::string w = orEmpty(env, where);
    double box[4] = {0, 0, 0, 0};
    if (bbox) {
        if (env->GetArrayLength(bbox) != 4) {
            throwIllegalArgument(env, "bbox must have 4 numbers");
            return nullptr;
        }
        env->GetDoubleArrayRegion(bbox, 0, 4, box);
    }
    std::lock_guard<std::mutex> lock(gLock);
    OGRLayer *l = layer(env, h, name);
    if (!l) return nullptr;
    CPLErrorReset();
    if (l->SetAttributeFilter(w.empty() ? nullptr : w.c_str()) != OGRERR_NONE) {
        throwGdalError(env, "bad where");
        return nullptr;
    }
    if (bbox) l->SetSpatialFilterRect(box[0], box[1], box[2], box[3]);
    std::string out = "[";
    bool first = true;
    l->ResetReading();
    for (OGRFeatureUniquePtr f(l->GetNextFeature()); f; f.reset(l->GetNextFeature())) {
        if (!first) out += ',';
        first = false;
        featureJson(out, f.get());
    }
    out += ']';
    l->SetAttributeFilter(nullptr);
    l->SetSpatialFilter(nullptr);
    if (CPLGetLastErrorType() == CE_Failure) {
        throwGdalError(env, "cannot read the features");
        return nullptr;
    }
    return fromUtf8(env, out);
}

// One feature as JSON, or null when there is no feature fid.
extern "C" JNIEXPORT jstring JNICALL VECTOR(feature)(JNIEnv *env, jclass, jlong h, jstring name, jlong fid) {
    std::lock_guard<std::mutex> lock(gLock);
    OGRLayer *l = layer(env, h, name);
    if (!l) return nullptr;
    OGRFeatureUniquePtr f(l->GetFeature(fid));
    if (!f) return nullptr;
    std::string out;
    featureJson(out, f.get());
    return fromUtf8(env, out);
}

// Adds a feature and returns its fid; a "fid" in the JSON is ignored.
extern "C" JNIEXPORT jlong JNICALL VECTOR(add)(JNIEnv *env, jclass, jlong h, jstring name, jstring feature) {
    if (!feature) {
        throwIllegalArgument(env, "feature is null");
        return 0;
    }
    const std::string text = toUtf8(env, feature);
    std::lock_guard<std::mutex> lock(gLock);
    OGRLayer *l = layer(env, h, name);
    if (!l) return 0;
    CPLErrorReset();
    OGRFeature f(l->GetLayerDefn());
    const std::string why = fill(&f, text);
    if (!why.empty()) {
        throwIllegalArgument(env, why.c_str());
        return 0;
    }
    if (l->CreateFeature(&f) != OGRERR_NONE) {
        throwGdalError(env, "cannot add the feature");
        return 0;
    }
    return f.GetFID();
}

// Changes the geometry and the properties the JSON names of feature fid.
extern "C" JNIEXPORT void JNICALL VECTOR(update)(JNIEnv *env, jclass, jlong h, jstring name, jlong fid,
                                                  jstring feature) {
    if (!feature) {
        throwIllegalArgument(env, "feature is null");
        return;
    }
    const std::string text = toUtf8(env, feature);
    std::lock_guard<std::mutex> lock(gLock);
    OGRLayer *l = layer(env, h, name);
    if (!l) return;
    CPLErrorReset();
    OGRFeatureUniquePtr f(l->GetFeature(fid));
    if (!f) {
        throwIllegalArgument(env, CPLSPrintf("there is no feature %lld", static_cast<long long>(fid)));
        return;
    }
    const std::string why = fill(f.get(), text);
    if (!why.empty()) {
        throwIllegalArgument(env, why.c_str());
        return;
    }
    if (l->SetFeature(f.get()) != OGRERR_NONE) throwGdalError(env, "cannot save the feature");
}

extern "C" JNIEXPORT void JNICALL VECTOR(delete)(JNIEnv *env, jclass, jlong h, jstring name, jlong fid) {
    std::lock_guard<std::mutex> lock(gLock);
    OGRLayer *l = layer(env, h, name);
    if (!l) return;
    CPLErrorReset();
    const OGRErr err = l->DeleteFeature(fid);
    if (err == OGRERR_NON_EXISTING_FEATURE) {
        throwIllegalArgument(env, CPLSPrintf("there is no feature %lld", static_cast<long long>(fid)));
    } else if (err != OGRERR_NONE) {
        throwGdalError(env, "cannot delete the feature");
    }
}

// what: 0 begins a transaction, 1 commits it, 2 rolls it back.
extern "C" JNIEXPORT void JNICALL VECTOR(transaction)(JNIEnv *env, jclass, jlong h, jint what) {
    std::lock_guard<std::mutex> lock(gLock);
    GDALDataset *ds = dataset(env, h);
    if (!ds) return;
    CPLErrorReset();
    const OGRErr err = what == 0   ? ds->StartTransaction(FALSE)
                       : what == 1 ? ds->CommitTransaction()
                                   : ds->RollbackTransaction();
    if (err != OGRERR_NONE) throwGdalError(env, what == 0 ? "cannot begin" : what == 1 ? "cannot commit" : "cannot roll back");
}

// Runs sql in the dataset's own dialect (SQLite for a GeoPackage) and returns
// the rows as a JSON array of features, empty for statements without rows.
extern "C" JNIEXPORT jstring JNICALL VECTOR(sql)(JNIEnv *env, jclass, jlong h, jstring sql) {
    if (!sql) {
        throwIllegalArgument(env, "sql is null");
        return nullptr;
    }
    const std::string q = toUtf8(env, sql);
    std::lock_guard<std::mutex> lock(gLock);
    GDALDataset *ds = dataset(env, h);
    if (!ds) return nullptr;
    CPLErrorReset();
    std::string out = "[";
    if (OGRLayer *rs = ds->ExecuteSQL(q.c_str(), nullptr, nullptr)) {
        bool first = true;
        for (auto &f : *rs) {
            if (!first) out += ',';
            first = false;
            featureJson(out, f.get());
        }
        ds->ReleaseResultSet(rs);
    }
    out += ']';
    if (CPLGetLastErrorType() == CE_Failure) {
        throwGdalError(env, "the SQL failed");
        return nullptr;
    }
    return fromUtf8(env, out);
}
