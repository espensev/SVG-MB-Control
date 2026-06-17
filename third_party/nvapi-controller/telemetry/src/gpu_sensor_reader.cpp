#include "gpu_telemetry/gpu_sensor_reader.h"

#include <utility>

namespace {

bool is_valid_gpu_index(const GpuProbe& probe, int gpu_index) {
    return gpu_index >= 0 && gpu_index < probe.gpu_count();
}

bool sample_with_mode(const GpuProbe& probe, int gpu_index,
                      GpuSampleMode mode, GpuSnapshot& snapshot) {
    snapshot.gpu_index = gpu_index;

    switch (mode) {
        case GpuSampleMode::ThermalFast:
            return probe.sample_thermal_fast(gpu_index, snapshot);
        case GpuSampleMode::Fast:
            return probe.sample_fast(gpu_index, snapshot);
        case GpuSampleMode::Medium:
            return probe.sample_medium(gpu_index, snapshot);
        case GpuSampleMode::Slow:
            return probe.sample_slow(gpu_index, snapshot);
        case GpuSampleMode::Rare:
            return probe.sample_rare(gpu_index, snapshot);
        case GpuSampleMode::Full: {
            bool a = probe.sample_fast(gpu_index, snapshot);
            bool b = probe.sample_slow(gpu_index, snapshot);
            return a || b;
        }
    }
    return false;
}

}  // namespace

GpuSensorReader::GpuSensorReader() = default;

GpuSensorReader::~GpuSensorReader() {
    shutdown();
}

GpuSensorReader::GpuSensorReader(GpuSensorReader&& other) noexcept
    : probe_(std::move(other.probe_)),
      initialized_(std::exchange(other.initialized_, false)) {}

GpuSensorReader& GpuSensorReader::operator=(GpuSensorReader&& other) noexcept {
    if (this == &other)
        return *this;

    shutdown();
    probe_ = std::move(other.probe_);
    initialized_ = std::exchange(other.initialized_, false);
    return *this;
}

bool GpuSensorReader::init(std::string& out_warning) {
    out_warning.clear();
    if (initialized_)
        return true;

    if (!probe_.init(out_warning))
        return false;

    initialized_ = true;
    return true;
}

void GpuSensorReader::shutdown() {
    if (!initialized_)
        return;

    probe_.shutdown();
    initialized_ = false;
}

bool GpuSensorReader::is_initialized() const {
    return initialized_;
}

int GpuSensorReader::gpu_count() const {
    return initialized_ ? probe_.gpu_count() : 0;
}

const GpuInfo* GpuSensorReader::gpu_info(int index) const {
    if (!initialized_ || !is_valid_gpu_index(probe_, index))
        return nullptr;
    return &probe_.gpu(index);
}

std::vector<GpuInfo> GpuSensorReader::gpu_info() const {
    std::vector<GpuInfo> info;
    if (!initialized_)
        return info;

    info.reserve(probe_.gpu_count());
    for (int i = 0; i < probe_.gpu_count(); ++i)
        info.push_back(probe_.gpu(i));

    return info;
}

bool GpuSensorReader::sample(int gpu_index, GpuSnapshot& out_snapshot,
                             GpuSampleMode mode) const {
    out_snapshot = {};
    if (!initialized_ || !is_valid_gpu_index(probe_, gpu_index))
        return false;

    return sample_with_mode(probe_, gpu_index, mode, out_snapshot);
}

std::vector<GpuSnapshot> GpuSensorReader::sample_all(GpuSampleMode mode) const {
    std::vector<GpuSnapshot> samples;
    if (!initialized_)
        return samples;

    samples.reserve(probe_.gpu_count());
    for (int i = 0; i < probe_.gpu_count(); ++i) {
        GpuSnapshot snapshot{};
        sample_with_mode(probe_, i, mode, snapshot);
        samples.push_back(snapshot);
    }

    return samples;
}
