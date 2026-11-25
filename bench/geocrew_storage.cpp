// geocrew_storage: GeoPackage speed and crash recovery in given directories,
// for deciding where projects live: /sdcard (through FUSE), app-private
// storage, or a removable card. See README.md.
//
// Usage: geocrew_storage <dir>...
// Exit status is the number of failed checks.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBulk = 100000;   // features written in one transaction
constexpr int kSingles = 1000;  // features committed one at a time, like saving each field point
constexpr int kBatch = 100;     // features per transaction in the crash test
constexpr int kCrashRounds = 20;
// Inserts that take longer than two minutes stop and report their rate. The
// deadline covers the inserts only, not the commit or the spatial index build.
constexpr double kDeadlineMs = 120000;

int failures = 0;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void report(const char *what, double ms, const std::string &note = "") {
    std::printf("%-34s %9.1f ms  %s\n", what, ms, note.c_str());
}

void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s (last GDAL error: %s)\n", what, CPLGetLastErrorMsg());
        failures++;
    }
}

// Removes a GeoPackage with its rollback journal, WAL and shared-memory files.
void removeDb(const std::string &path) {
    for (const char *suffix : {"", "-journal", "-wal", "-shm"}) VSIUnlink((path + suffix).c_str());
}

bool addFeature(OGRLayer *layer, long long i, std::mt19937 &rng) {
    std::uniform_real_distribution<double> d(0.0, 1.0);
    OGRFeature f(layer->GetLayerDefn());
    f.SetField(0, CPLSPrintf("P%06lld", i));
    f.SetField(1, 100.0 + static_cast<double>(i % 900));
    OGRPoint p(113.5 + d(rng), 29.5 + d(rng));
    f.SetGeometry(&p);
    return layer->CreateFeature(&f) == OGRERR_NONE;
}

std::unique_ptr<GDALDataset> create(const std::string &path) {
    removeDb(path);
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
    std::unique_ptr<GDALDataset> ds(driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!ds) return nullptr;
    OGRSpatialReference srs;
    srs.importFromEPSG(4490);
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRLayer *layer = ds->CreateLayer("points", &srs, wkbPoint, nullptr);
    OGRFieldDefn name("name", OFTString), elev("elev", OFTReal);
    if (!layer || layer->CreateField(&name) != OGRERR_NONE || layer->CreateField(&elev) != OGRERR_NONE) return nullptr;
    return ds;
}

// The first value of the first row of a query, or "" if it fails.
std::string queryValue(GDALDataset *ds, const char *sql) {
    std::string out;
    if (OGRLayer *rs = ds->ExecuteSQL(sql, nullptr, nullptr)) {
        OGRFeatureUniquePtr f(rs->GetNextFeature());
        if (f) out = f->GetFieldAsString(0);
        ds->ReleaseResultSet(rs);
    }
    return out;
}

struct State {
    bool ok = false;
    long long rows = -1;
};

// Opens the file for writing, as the app would (this also rolls back a hot
// journal), and checks SQLite's integrity and that the spatial index has
// exactly one entry per row.
State inspect(const std::string &path) {
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    if (!ds) {
        std::printf("  cannot open: %s\n", CPLGetLastErrorMsg());
        return {};
    }
    const std::string integrity = queryValue(ds.get(), "PRAGMA integrity_check");
    const long long rows = std::atoll(queryValue(ds.get(), "SELECT count(*) FROM points").c_str());
    const long long indexed = std::atoll(queryValue(ds.get(), "SELECT count(*) FROM rtree_points_geom").c_str());
    const bool ok = integrity == "ok" && rows == indexed;
    if (!ok) std::printf("  integrity \"%s\", rows %lld, indexed %lld\n", integrity.c_str(), rows, indexed);
    return {ok, rows};
}

// Writes up to kBulk features into a new file in one transaction, as an import
// does, and returns how many it wrote. A slow run stops at the deadline and
// reports its rate instead. cacheMb sets SQLite's page cache; nullptr keeps
// SQLite's default of 2 MB.
int bulk(const std::string &path, const char *cacheMb) {
    std::mt19937 rng(1);
    auto t0 = Clock::now();
    CPLSetConfigOption("OGR_SQLITE_CACHE", cacheMb);
    std::unique_ptr<GDALDataset> ds = create(path);
    CPLSetConfigOption("OGR_SQLITE_CACHE", nullptr);
    check(ds != nullptr, "create GeoPackage");
    if (!ds) return 0;
    OGRLayer *layer = ds->GetLayer(0);
    int written = 0;
    bool late = false;
    check(ds->StartTransaction() == OGRERR_NONE, "bulk: start transaction");
    for (int i = 0; i < kBulk && !late; i++) {
        written += addFeature(layer, i, rng);
        late = i % 1000 == 999 && msSince(t0) > kDeadlineMs;
    }
    check(ds->CommitTransaction() == OGRERR_NONE, "bulk commit");
    // Queried only now: SQL run before the inserts makes GDAL build the spatial
    // index up front and update it on every insert, which is far slower.
    // GDAL turns synchronous off for a file it creates; reopened files get SQLite's default.
    std::printf("  new file: journal_mode %s, synchronous %s, cache_size %s\n",
                queryValue(ds.get(), "PRAGMA journal_mode").c_str(), queryValue(ds.get(), "PRAGMA synchronous").c_str(),
                queryValue(ds.get(), "PRAGMA cache_size").c_str());
    ds.reset();
    const double ms = msSince(t0);
    report(cacheMb ? CPLSPrintf("write 100k, %s MB cache", cacheMb) : "write 100k, default cache", ms,
           late ? CPLSPrintf("stopped at the deadline after %d, %.0f per second", written, written / (ms / 1000)) : "");
    return written;
}

void speed(const std::string &dir) {
    const std::string path = dir + "/storage.gpkg";
    bulk(path, "256");
    const int imported = bulk(path, nullptr);  // the file the next steps use

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
    check(ds != nullptr, "reopen for writing");
    if (!ds) return;
    std::printf("  reopened: journal_mode %s, synchronous %s\n", queryValue(ds.get(), "PRAGMA journal_mode").c_str(),
                queryValue(ds.get(), "PRAGMA synchronous").c_str());
    OGRLayer *layer = ds->GetLayer(0);
    std::mt19937 rng(2);
    int saved = 0;
    bool late = false;
    auto t0 = Clock::now();
    for (int i = 0; i < kSingles && !late; i++) {
        if (ds->StartTransaction() == OGRERR_NONE && addFeature(layer, kBulk + i, rng) &&
            ds->CommitTransaction() == OGRERR_NONE)
            saved++;
        late = msSince(t0) > kDeadlineMs;
    }
    const double ms = msSince(t0);
    report("single-feature commits", ms,
           CPLSPrintf("%d done, %.2f ms each%s", saved, ms / (saved ? saved : 1), late ? ", stopped at the deadline" : ""));
    check(late || saved == kSingles, "single commits");
    ds.reset();

    t0 = Clock::now();
    ds.reset(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR));
    int n = 0;
    for (auto &f : *ds->GetLayer(0)) {
        (void)f;
        n++;
    }
    report("read all", msSince(t0), CPLSPrintf("%d features", n));
    check(n == imported + saved, "read back every feature");
    ds.reset();
    removeDb(path);
}

// A child process commits batches of features until it is killed at a random
// moment. Afterwards the file must pass the checks in inspect() and hold every
// committed batch and nothing else. The last batch may have committed just
// before its count reached the pipe, so one batch more than reported is fine.
void crash(const std::string &dir) {
    const std::string path = dir + "/crash.gpkg";
    if (create(path) == nullptr) {
        check(false, "create crash-test GeoPackage");
        return;
    }
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> delayMs(20, 600);
    long long before = 0;
    int recovered = 0;
    auto t0 = Clock::now();
    for (int round = 0; round < kCrashRounds; round++) {
        int fds[2];
        if (pipe(fds) != 0) {
            check(false, "pipe");
            return;
        }
        const pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            std::unique_ptr<GDALDataset> ds(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE));
            if (!ds) _exit(10);
            OGRLayer *layer = ds->GetLayer(0);
            long long committed = std::atoll(queryValue(ds.get(), "SELECT count(*) FROM points").c_str());
            std::mt19937 childRng(round);
            for (;;) {
                if (ds->StartTransaction() != OGRERR_NONE) _exit(13);
                for (int i = 0; i < kBatch; i++) addFeature(layer, committed + i, childRng);
                if (ds->CommitTransaction() != OGRERR_NONE) _exit(11);
                committed += kBatch;
                if (write(fds[1], &committed, sizeof committed) != sizeof committed) _exit(12);
            }
        }
        close(fds[1]);
        usleep(static_cast<useconds_t>(delayMs(rng)) * 1000);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        long long reported = before, value = 0;
        while (read(fds[0], &value, sizeof value) == sizeof value) reported = value;
        close(fds[0]);
        if (!WIFSIGNALED(status)) {
            std::printf("  round %d: writer stopped by itself, status %d\n", round,
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            continue;
        }
        const State s = inspect(path);
        if (s.ok && (s.rows == reported || s.rows == reported + kBatch)) {
            recovered++;
        } else {
            std::printf("  round %d: rows %lld, reported %lld\n", round, s.rows, reported);
        }
        if (s.rows >= 0) before = s.rows;
    }
    report("crash rounds recovered", msSince(t0),
           CPLSPrintf("%d of %d, %lld rows at the end", recovered, kCrashRounds, before));
    check(recovered == kCrashRounds, "crash recovery");
    removeDb(path);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <dir>...\n", argv[0]);
        return 100;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the results if a later step crashes
    GDALAllRegister();
    for (int i = 1; i < argc; i++) {
        const std::string dir = argv[i];
        VSIMkdirRecursive(dir.c_str(), 0755);
        for (const char *journal : {"DELETE", "WAL"}) {
            std::printf("== %s, journal %s\n", dir.c_str(), journal);
            CPLSetConfigOption("OGR_SQLITE_JOURNAL", journal);
            speed(dir);
            crash(dir);
        }
    }
    std::printf("%s: %d failed checks\n", failures ? "FAILED" : "OK", failures);
    return failures;
}
