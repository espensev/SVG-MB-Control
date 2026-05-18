#include "service_probe.h"

#include "amd_reader.h"
#include "control_config.h"
#include "control_scheduler.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "json_io.h"
#include "runtime_write_policy.h"

#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace svg_mb_control {

namespace {

struct ProbeCheck {
    std::string name;
    bool required = true;
    bool ok = false;
    std::string detail;
};

ProbeCheck CheckAmd() {
    ProbeCheck check{"amd_telemetry", true, false, ""};
    try {
        AmdReader reader;
        const auto snapshot = reader.Sample();
        check.ok = reader.available() && snapshot.available;
        if (check.ok) {
            check.detail = "cpu=\"" + snapshot.cpu_name + "\" samples=" +
                           std::to_string(snapshot.samples.size());
        } else {
            const std::string warn = !snapshot.last_warning.empty()
                                         ? snapshot.last_warning
                                         : reader.init_warning();
            check.detail = warn.empty() ? "AMD telemetry unavailable" : warn;
        }
    } catch (const std::exception& error) {
        check.detail = std::string("AMD reader threw: ") + error.what();
    }
    return check;
}

ProbeCheck CheckGpu() {
    // Advisory only: this controller treats GPU input as optional, so a
    // service can be viable on a machine without readable GPU telemetry.
    ProbeCheck check{"gpu_telemetry", false, false, ""};
    try {
        GpuReader reader;
        const auto sample = reader.Sample();
        check.ok = reader.available() && sample.available;
        if (check.ok) {
            check.detail = "gpu=\"" + sample.gpu_name + "\" core_c=" +
                           std::to_string(sample.core_c);
        } else {
            const std::string warn = !sample.last_warning.empty()
                                         ? sample.last_warning
                                         : reader.init_warning();
            check.detail = warn.empty() ? "GPU telemetry unavailable (optional)"
                                        : warn;
        }
    } catch (const std::exception& error) {
        check.detail = std::string("GPU reader threw: ") + error.what();
    }
    return check;
}

// Reads SIO fan state and verifies a clean fan-writer open/close without ever
// issuing a duty write. Reports whether the no-write lifecycle left a
// pending-write sidecar behind.
void CheckSioAndLifecycle(const ControlConfig* config,
                          const std::filesystem::path& runtime_home,
                          ProbeCheck* sio_check,
                          ProbeCheck* lifecycle_check) {
    sio_check->name = "sio_fan_state";
    sio_check->required = true;
    lifecycle_check->name = "no_write_lifecycle";
    lifecycle_check->required = true;

    const std::filesystem::path pending_path =
        runtime_home / "pending_writes.json";
    std::error_code ec;
    const bool pending_before =
        std::filesystem::exists(pending_path, ec);

    try {
        const RuntimeWritePolicy runtime_policy =
            ResolveRuntimeWritePolicy(config);
        {
            std::unique_ptr<FanWriter> fan_writer =
                CreateFanWriter(runtime_policy);
            const FanScanResult scan = fan_writer->ReadAllChannels();
            sio_check->ok = scan.ok();
            if (scan.ok()) {
                sio_check->detail = "backend=\"" + fan_writer->BackendLabel() +
                                    "\" fans=" +
                                    std::to_string(scan.fans.size());
            } else {
                sio_check->detail = scan.detail.empty()
                                        ? "SIO fan scan failed"
                                        : scan.detail;
            }
            // fan_writer is destroyed here: exercises a clean close with no
            // ApplyDuty/RestoreSavedState ever called.
        }
    } catch (const std::exception& error) {
        sio_check->detail = std::string("fan writer threw: ") + error.what();
    }

    const bool pending_after =
        std::filesystem::exists(pending_path, ec);
    lifecycle_check->ok = pending_after == pending_before;
    if (lifecycle_check->ok) {
        lifecycle_check->detail =
            "no fan-duty write issued; fan writer opened and closed cleanly";
    } else {
        lifecycle_check->detail =
            "probe unexpectedly created pending_writes.json";
    }
}

ProbeCheck CheckRuntimeHomeWritable(
    const std::filesystem::path& runtime_home) {
    ProbeCheck check{"runtime_home_writable", true, false, ""};
    std::error_code ec;
    std::filesystem::create_directories(runtime_home, ec);
    const std::filesystem::path probe_file =
        runtime_home / "service_probe.tmp";
    {
        std::ofstream out(probe_file, std::ios::trunc);
        if (!out.is_open()) {
            check.detail = "could not open " + probe_file.string() +
                           " for writing";
            return check;
        }
        out << "service-probe writability check\n";
    }
    std::error_code remove_ec;
    std::filesystem::remove(probe_file, remove_ec);
    check.ok = true;
    check.detail = "wrote and removed " + probe_file.string();
    return check;
}

}  // namespace

int RunServiceProbe(const std::filesystem::path& runtime_home,
                    const ControlConfig* config,
                    bool json_output) {
    std::vector<ProbeCheck> checks;
    checks.push_back(CheckAmd());
    checks.push_back(CheckGpu());

    ProbeCheck sio_check;
    ProbeCheck lifecycle_check;
    CheckSioAndLifecycle(config, runtime_home, &sio_check, &lifecycle_check);
    checks.push_back(sio_check);
    checks.push_back(CheckRuntimeHomeWritable(runtime_home));
    checks.push_back(lifecycle_check);

    bool overall_ok = true;
    for (const auto& check : checks) {
        if (check.required && !check.ok) {
            overall_ok = false;
        }
    }

    if (json_output) {
        nlohmann::json payload = MakeSchemaObject(1u);
        payload["generated_time"] =
            FormatLocalIso8601(std::chrono::system_clock::now());
        payload["runtime_home"] = runtime_home.string();
        payload["overall"] = overall_ok ? "pass" : "fail";
        payload["checks"] = nlohmann::json::array();
        for (const auto& check : checks) {
            payload["checks"].push_back({
                {"name", check.name},
                {"required", check.required},
                {"ok", check.ok},
                {"detail", check.detail},
            });
        }
        std::cout << payload.dump(2) << '\n';
    } else {
        std::cout << "svg-mb-control: service-probe "
                  << (overall_ok ? "pass" : "fail") << '\n'
                  << "  runtime_home: " << runtime_home.string() << '\n';
        for (const auto& check : checks) {
            std::cout << "  [" << (check.ok ? "ok " : "FAIL")
                      << (check.required ? "" : " advisory") << "] "
                      << check.name << ": " << check.detail << '\n';
        }
    }

    return overall_ok ? 0 : 1;
}

}  // namespace svg_mb_control
