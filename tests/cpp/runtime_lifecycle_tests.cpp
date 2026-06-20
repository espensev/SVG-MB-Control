// Unit tests for the FEAT-0023 profile-switch request primitive
// (RequestRuntimeProfileSwitch / TakeRuntimeProfileSwitchRequest), a take-once
// request file modeled on the breaker-reset primitive.

#include "runtime_lifecycle.h"

#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using svg_mb_control::RequestRuntimeProfileSwitch;
using svg_mb_control::RuntimeProfileSwitchRequestPath;
using svg_mb_control::TakeRuntimeProfileSwitchRequest;

// A unique temp directory used as a runtime_home; removed on destruct.
class TempHome {
  public:
    TempHome() {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec || base.empty()) {
            throw std::runtime_error("temp_directory_path failed in TempHome");
        }
        path_ = base / ("svg_mb_control_lifecycle_test_" + UniqueTempSuffix());
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            throw std::runtime_error("create_directories failed for " +
                                     path_.string());
        }
    }
    ~TempHome() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempHome(const TempHome&) = delete;
    TempHome& operator=(const TempHome&) = delete;
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

bool RequestFileExists(const TempHome& home) {
    std::error_code ec;
    return std::filesystem::exists(
        RuntimeProfileSwitchRequestPath(home.path()), ec);
}

void TestRoundTrip() {
    TempHome home;
    ExpectTrue(RequestRuntimeProfileSwitch(home.path(), "quiet"),
               "request writes the profile-switch file");
    ExpectTrue(RequestFileExists(home),
               "the request file exists after a request");
    const auto taken = TakeRuntimeProfileSwitchRequest(home.path());
    ExpectTrue(taken.has_value(), "take returns the request");
    if (taken.has_value()) {
        ExpectEqual(taken->profile_name, "quiet",
                    "take returns the requested profile name");
        ExpectEqual(taken->parse_error, "",
                    "a well-formed request has no parse error");
    }
    ExpectFalse(RequestFileExists(home),
                "take clears the request file (take-once)");
}

void TestTakeWithNoRequestReturnsNullopt() {
    TempHome home;
    ExpectFalse(TakeRuntimeProfileSwitchRequest(home.path()).has_value(),
                "take with no request file returns nullopt");
}

void TestTakeOnce() {
    TempHome home;
    RequestRuntimeProfileSwitch(home.path(), "perf");
    ExpectTrue(TakeRuntimeProfileSwitchRequest(home.path()).has_value(),
               "first take returns the request");
    ExpectFalse(TakeRuntimeProfileSwitchRequest(home.path()).has_value(),
                "second take returns nullopt (take-once)");
}

void TestEmptyProfileNameRefused() {
    TempHome home;
    ExpectFalse(RequestRuntimeProfileSwitch(home.path(), ""),
                "an empty profile name is refused");
    ExpectFalse(RequestFileExists(home),
                "no request file is written for an empty profile name");
}

void TestMalformedRequestSetsParseError() {
    TempHome home;
    {
        std::ofstream out(RuntimeProfileSwitchRequestPath(home.path()),
                          std::ios::binary | std::ios::trunc);
        out << R"({"schema_version":1,"requested_at":"now"})";  // no "profile"
    }
    const auto taken = TakeRuntimeProfileSwitchRequest(home.path());
    ExpectTrue(taken.has_value(),
               "take returns a value for a present-but-malformed request");
    if (taken.has_value()) {
        ExpectFalse(taken->parse_error.empty(),
                    "a malformed request sets parse_error");
    }
}

}  // namespace

int main() {
    TestRoundTrip();
    TestTakeWithNoRequestReturnsNullopt();
    TestTakeOnce();
    TestEmptyProfileNameRefused();
    TestMalformedRequestSetsParseError();
    if (g_failures == 0) {
        std::cout << "runtime_lifecycle_tests: all passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
