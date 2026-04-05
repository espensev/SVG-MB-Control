#pragma once

#include <memory>
#include <string>

namespace svg_mb_control {

struct GpuTempSample {
    bool available = false;      // true when a live sample was produced
    double core_c = 0.0;
    double memjn_c = 0.0;
    double hotspot_c = 0.0;      // 0.0 when sensor absent (e.g., RTX 5090)
    std::string gpu_name;
    std::string last_warning;
};

// RAII wrapper around the nvapi-controller `GpuSensorReader`. Reads GPU
// core/memjn/hotspot temperatures from NVAPI. Construction never throws;
// failure to initialize leaves `available()` false and subsequent
// `Sample()` calls return an empty sample with `available=false`.
class GpuReader {
  public:
    GpuReader();
    ~GpuReader();

    GpuReader(const GpuReader&) = delete;
    GpuReader& operator=(const GpuReader&) = delete;

    // Returns true when at least one GPU is enumerated and initialization
    // succeeded.
    bool available() const;

    // Diagnostic message from initialization. Empty on clean init.
    std::string init_warning() const;

    // Samples GPU 0 (first enumerated GPU). Returns an empty sample with
    // available=false on any failure.
    GpuTempSample Sample();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace svg_mb_control
