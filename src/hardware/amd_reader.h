#pragma once

#include <cstdint>
#include <limits>
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

    // FEAT-0006 read-only RAPL package-energy evidence (REQ-CPUEFF-02). Raw,
    // quarantined, default off. Average watts is derived later by the analyzer,
    // not here. acquisition is one of: "disabled" (off, the default),
    // "unavailable" (enabled but the read failed / bin hash mismatched / RAPL
    // absent), "quarantine" (collected but not yet trusted), "validated" (only
    // after the post-implementation Evaluation; never set automatically in v1).
    // The sample id increments only when a new ~1 s resource-window sample is
    // taken; intervening control ticks mirror the latest, so the analyzer must
    // de-duplicate on a distinct nonzero id. delta/window are blank (NaN) when
    // unavailable or when the implausibility guard fires (no false zero).
    std::string pkg_energy_acquisition = "disabled";
    std::uint64_t pkg_energy_sample_id = 0u;
    double pkg_energy_window_ms = std::numeric_limits<double>::quiet_NaN();
    double pkg_energy_delta_uj = std::numeric_limits<double>::quiet_NaN();

    // FEAT-0006 read-only APERF/MPERF cycle evidence (REQ-CPUEFF-01 work
    // numerator + REQ-CPUEFF-03 effective-frequency context). Raw delivered
    // cycles (dAPERF) and reference cycles (dMPERF) per ~1 s window, read
    // per-core under a transient affinity pin. Same provenance states as
    // pkg_energy; the analyzer derives effective frequency (dAPERF/dMPERF x P0).
    // Deltas are blank (NaN) on baseline / unavailable / guard-blank (no false
    // zero).
    std::string cpu_cycles_acquisition = "disabled";
    std::uint64_t cpu_cycles_sample_id = 0u;
    double cpu_cycles_window_ms = std::numeric_limits<double>::quiet_NaN();
    double cpu_aperf_delta = std::numeric_limits<double>::quiet_NaN();
    double cpu_mperf_delta = std::numeric_limits<double>::quiet_NaN();
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
