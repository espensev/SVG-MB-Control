#include "app/app_diagnose.h"

#include "amd_reader.h"
#include "control_config.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "hardware_access_status.h"
#include "runtime_snapshot.h"
#include "runtime_write_policy.h"

#include <iostream>
#include <memory>

namespace svg_mb_control {

std::string SampleDirectSnapshotJson(const ControlConfig* config) {
    const RuntimeWritePolicy runtime_policy =
        ResolveRuntimeWritePolicy(config);
    std::unique_ptr<FanWriter> writer = CreateFanWriter(runtime_policy);
    AmdReader amd_reader;
    GpuReader gpu_reader;
    const RuntimeSnapshot snapshot = SampleDirectRuntimeSnapshot(
        amd_reader, gpu_reader, *writer, runtime_policy);
    return SerializeRuntimeSnapshotJson(snapshot);
}

int RunDiagnoseAmd() {
    AmdReader reader;
    const HardwareAccessState read_state =
        reader.available() ? HardwareAccessState::kAvailable
                           : HardwareAccessState::kUnavailable;
    std::cout << "hardware_access.read_state: "
              << HardwareAccessStateName(read_state) << '\n';
    std::cout << "amd_reader.available: "
              << (reader.available() ? "true" : "false") << '\n';
    std::cout << "amd_reader.init_warning: \""
              << reader.init_warning() << "\"\n";
    const auto snapshot = reader.Sample();
    std::cout << "sample.available: "
              << (snapshot.available ? "true" : "false") << '\n';
    std::cout << "sample.cpu_name: \"" << snapshot.cpu_name << "\"\n";
    std::cout << "sample.transport_path: \""
              << snapshot.transport_path << "\"\n";
    std::cout << "sample.last_warning: \""
              << snapshot.last_warning << "\"\n";
    std::cout << "sample.count: " << snapshot.samples.size() << '\n';
    for (std::size_t sample_index = 0u;
         sample_index < snapshot.samples.size();
         ++sample_index) {
        const auto& sample = snapshot.samples[sample_index];
        std::cout << "sample[" << sample_index << "].label: \""
                  << sample.label << "\"\n";
        std::cout << "sample[" << sample_index
                  << "].temperature_c: " << sample.temperature_c << '\n';
    }
    return snapshot.available ? 0 : 1;
}

int RunDiagnoseGpu() {
    GpuReader reader;
    std::cout << "gpu_reader.available: "
              << (reader.available() ? "true" : "false") << '\n';
    std::cout << "gpu_reader.init_warning: \""
              << reader.init_warning() << "\"\n";
    const auto sample = reader.Sample();
    std::cout << "sample.available: "
              << (sample.available ? "true" : "false") << '\n';
    std::cout << "sample.gpu_name: \"" << sample.gpu_name << "\"\n";
    std::cout << "sample.core_c: " << sample.core_c << '\n';
    std::cout << "sample.memjn_c: " << sample.memjn_c << '\n';
    std::cout << "sample.hotspot_c: " << sample.hotspot_c << '\n';
    std::cout << "sample.last_warning: \""
              << sample.last_warning << "\"\n";
    return sample.available ? 0 : 1;
}

}  // namespace svg_mb_control
