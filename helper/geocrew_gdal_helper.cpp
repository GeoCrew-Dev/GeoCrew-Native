// geocrew-gdal-helper: the server's GDAL helper (ADR 0013 in the main GeoCrew
// repository). The Go server runs it as a child process for exports and
// coordinate conversion, so a crash in GDAL cannot take the sync service down.
//
// It takes the arguments of the `gdal` command line and prints what the
// algorithm prints, for example:
//   geocrew-gdal-helper vector convert in.geojson out.gpkg
//   geocrew-gdal-helper vector info --format=json out.gpkg
// --version prints the library versions. PROJ's and GDAL's data files are read
// from the "data" directory next to the executable, unless PROJ_DATA or
// GDAL_DATA is set. Exit status: 0 done, 1 the algorithm failed, 2 bad arguments.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdalalgorithm.h>
#include <geos_c.h>
#include <ogr_srs_api.h>
#include <proj.h>

int main(int argc, char **argv) {
    char exe[4096];
    if (CPLGetExecPath(exe, sizeof exe)) {
        const std::string data = CPLFormFilenameSafe(CPLGetDirnameSafe(exe).c_str(), "data", nullptr);
        if (!std::getenv("PROJ_DATA")) {
            const char *const paths[] = {data.c_str(), nullptr};
            OSRSetPROJSearchPaths(paths);
        }
        if (!std::getenv("GDAL_DATA")) CPLSetConfigOption("GDAL_DATA", data.c_str());
    }
    GDALAllRegister();
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("GDAL %s, PROJ %s, GEOS %s\n", GDALVersionInfo("RELEASE_NAME"), proj_info().version, GEOSversion());
        return 0;
    }
    CPLStringList args;
    for (int i = 1; i < argc; i++) args.AddString(argv[i]);

    GDALAlgorithmRegistryH registry = GDALGetGlobalAlgorithmRegistry();
    GDALAlgorithmH root = GDALAlgorithmRegistryInstantiateAlg(registry, "gdal");
    GDALAlgorithmRegistryRelease(registry);
    if (!root) {
        std::fprintf(stderr, "GDAL algorithms are not available\n");
        return 1;
    }
    int status = 0;
    if (!GDALAlgorithmParseCommandLineArguments(root, args.List())) {
        std::fprintf(stderr, "invalid arguments: %s\n", CPLGetLastErrorMsg());
        status = 2;
    } else {
        GDALAlgorithmH alg = GDALAlgorithmGetActualAlgorithm(root);
        if (GDALAlgorithmRun(alg, nullptr, nullptr) && GDALAlgorithmFinalize(alg)) {
            if (GDALAlgorithmArgH arg = GDALAlgorithmGetArg(alg, "output-string")) {
                std::fputs(GDALAlgorithmArgGetAsString(arg), stdout);
                GDALAlgorithmArgRelease(arg);
            }
        } else {
            std::fprintf(stderr, "failed: %s\n", CPLGetLastErrorMsg());
            status = 1;
        }
        GDALAlgorithmRelease(alg);
    }
    GDALAlgorithmRelease(root);
    return status;
}
