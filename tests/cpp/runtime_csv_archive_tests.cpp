#include "runtime_csv_archive.h"

#include "test_helpers.h"

#include <filesystem>
#include <string>

namespace {

std::filesystem::path MakeTempRuntimeHome(const std::string& name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("svg_mb_control_runtime_csv_archive_tests_" + name + "_" +
         UniqueTempSuffix());
    std::filesystem::create_directories(root / "logs" / "archive");
    return root;
}

void TestMirrorOpenFailureRecordsSink() {
    const auto runtime_home = MakeTempRuntimeHome("mirror_open");
    svg_mb_control::RuntimeArtifactNaming naming;
    naming.latest_csv_name = "archive";

    svg_mb_control::RuntimeCsvLogger logger(runtime_home,
                                            0u,
                                            0u,
                                            1u,
                                            naming);

    ExpectFalse(logger.Open("control-loop", "a,b"),
                "logger open fails when fixed mirror path is a directory");
    ExpectEqual(logger.last_error_sink(), "mirror_open",
                "mirror open failure records sink");
    ExpectTrue(logger.last_error_detail().find("fixed CSV mirror") !=
                   std::string::npos,
               "mirror open failure records detail");

    std::filesystem::remove_all(runtime_home);
}

void TestManifestWriteFailureReturnsFromWriteRow() {
    const auto runtime_home = MakeTempRuntimeHome("manifest_write");
    svg_mb_control::RuntimeArtifactNaming naming;
    naming.latest_manifest_name = "blocked_manifest.json";
    std::filesystem::create_directory(runtime_home / "logs" /
                                      naming.latest_manifest_name);

    svg_mb_control::RuntimeCsvLogger logger(runtime_home,
                                            0u,
                                            0u,
                                            1u,
                                            naming);
    ExpectTrue(logger.Open("control-loop", "a,b"),
               "logger stays open when latest manifest write fails");

    for (int i = 1; i < 100; ++i) {
        ExpectTrue(logger.WriteRow(std::to_string(i) + ",ok"),
                   "rows before manifest interval write successfully");
    }

    ExpectFalse(logger.WriteRow("100,fail"),
                "manifest write failure is returned by WriteRow");
    ExpectEqual(logger.last_error_sink(), "manifest_write",
                "manifest write failure records sink");
    ExpectTrue(logger.last_error_detail().find("latest_manifest") !=
                   std::string::npos,
               "manifest write failure records latest manifest detail");

    logger.Close();
    std::filesystem::remove_all(runtime_home);
}

void TestSuccessfulWriteClearsLastError() {
    const auto runtime_home = MakeTempRuntimeHome("success");
    svg_mb_control::RuntimeCsvLogger logger(runtime_home, 0u, 0u, 1u);

    ExpectTrue(logger.Open("control-loop", "a,b"), "logger opens");
    ExpectTrue(logger.WriteRow("1,ok"), "row write succeeds");
    ExpectEqual(logger.last_error_sink(), "",
                "successful row write clears error sink");
    ExpectEqual(logger.last_error_detail(), "",
                "successful row write clears error detail");

    logger.Close();
    std::filesystem::remove_all(runtime_home);
}

}  // namespace

int main() {
    TestMirrorOpenFailureRecordsSink();
    TestManifestWriteFailureReturnsFromWriteRow();
    TestSuccessfulWriteClearsLastError();
    return g_failures == 0 ? 0 : 1;
}
