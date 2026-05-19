#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace svg_mb_control {

struct AmdTemperatureSample {
    std::string label;
    double temperature_c = 0.0;
    std::uint32_t sensor_index = 0u;
    std::uint32_t raw_value = 0u;
};

struct AmdSnapshot {
    bool available = false;
    std::vector<AmdTemperatureSample> samples;
    std::string cpu_name;
    std::string transport_path;
    std::string last_warning;
};

// RAII wrapper around the current private AMD SMN reader. Construction never
// throws; failure to initialize leaves `available()` false and `Sample()`
// returns an empty snapshot with `available=false`.
class AmdReader {
  public:
    AmdReader();
    ~AmdReader();

    AmdReader(const AmdReader&) = delete;
    AmdReader& operator=(const AmdReader&) = delete;

    bool available() const;
    std::string init_warning() const;

    // Samples current AMD temperatures. The returned reference aliases an
    // internal buffer that is overwritten on the next Sample() call on this
    // reader; copy it if you need to retain it past that point. Reusing the
    // buffer avoids reallocating the samples vector on every steady-state
    // control tick.
    const AmdSnapshot& Sample();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AmdSnapshot sample_buffer_;
};

}  // namespace svg_mb_control
