#include "gpu_reader.h"

#ifdef SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED
#include "gpu_telemetry/gpu_sensor_reader.h"
#endif

namespace svg_mb_control {

#ifdef SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED

struct GpuReader::Impl {
    GpuSensorReader reader;
    bool initialized = false;
    std::string init_warning;
    std::string gpu_name;
};

GpuReader::GpuReader() : impl_(std::make_unique<Impl>()) {
    std::string warning;
    const bool ok = impl_->reader.init(warning);
    impl_->initialized = ok;
    impl_->init_warning = warning;
    if (ok && impl_->reader.gpu_count() > 0) {
        if (const GpuInfo* info = impl_->reader.gpu_info(0)) {
            impl_->gpu_name = info->name;
        }
    }
}

GpuReader::~GpuReader() {
    if (impl_ && impl_->initialized) {
        impl_->reader.shutdown();
    }
}

bool GpuReader::available() const {
    return impl_ != nullptr && impl_->initialized &&
           impl_->reader.gpu_count() > 0;
}

std::string GpuReader::init_warning() const {
    return impl_ ? impl_->init_warning : std::string();
}

GpuTempSample GpuReader::Sample() {
    GpuTempSample out;
    if (!available()) {
        out.last_warning = impl_ ? impl_->init_warning : "not initialized";
        return out;
    }
    GpuSnapshot snap;
    if (!impl_->reader.sample(0, snap, GpuSampleMode::ThermalFast)) {
        out.last_warning = "sample failed";
        return out;
    }
    out.available = true;
    out.core_c = snap.core_c;
    out.memjn_c = snap.memjn_c;
    out.hotspot_c = snap.hotspot_c;
    out.gpu_name = impl_->gpu_name;
    return out;
}

#else  // SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED

struct GpuReader::Impl {
    std::string init_warning =
        "gpu_telemetry not linked at build time";
};

GpuReader::GpuReader() : impl_(std::make_unique<Impl>()) {}
GpuReader::~GpuReader() = default;

bool GpuReader::available() const { return false; }

std::string GpuReader::init_warning() const {
    return impl_ ? impl_->init_warning : std::string();
}

GpuTempSample GpuReader::Sample() {
    GpuTempSample out;
    out.last_warning = init_warning();
    return out;
}

#endif  // SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED

}  // namespace svg_mb_control
