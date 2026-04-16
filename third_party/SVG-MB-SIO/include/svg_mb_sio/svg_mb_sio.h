#ifndef SVG_MB_SIO_H
#define SVG_MB_SIO_H

/*
 * svg_mb_sio.h - Public interface for motherboard Super I/O fan control.
 *
 * Consumer includes this one header, sees no raw NCT6701D register details,
 * and links against svg_mb_sio.lib.
 *
 * Scope:
 *   - NCT6701D fan reads, writes, restore operations
 *   - NCT6701D voltage and SIO temperature reads
 *   - Raw register reads for bench and verification workflows
 *
 * Non-goals:
 *   - AMD SMN transport
 *   - Thermal policy loops or external verifier integration
 *   - Public dependency on LibreHardwareMonitor or PawnIO-specific types
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct MbSioControllerImpl;

enum class MbSioTransport { none, pawnio_lpc };

struct MbFanSnapshot {
    std::uint32_t channel = 0;
    std::uint16_t rpm = 0;
    std::uint16_t tach_raw = 0;
    std::uint8_t duty_raw = 0;
    std::uint8_t mode_raw = 0;
    double duty_percent = 0.0;
    bool tach_valid = false;
    bool manual_override = false;
    std::string label;
};

struct MbVoltageSnapshot {
    std::uint32_t index = 0;
    double voltage_v = 0.0;
    std::uint8_t raw = 0;
    std::string label;
};

struct MbSioTemperatureSnapshot {
    std::uint32_t index = 0;
    double temperature_c = 0.0;
    std::uint8_t raw = 0;
    std::uint8_t half_raw = 0;
    bool valid = false;
    std::string label;
};

struct MbSioDeviceDescriptor {
    std::uint16_t chip_id = 0;
    std::uint8_t chip_revision = 0;
    std::uint32_t fan_count = 0;
    std::uint32_t voltage_count = 0;
    std::uint32_t temp_count = 0;
    MbSioTransport transport = MbSioTransport::none;
    std::string transport_path;
    std::uint16_t hwm_base = 0;
    std::uint16_t index_port = 0;
    std::uint32_t generation = 0;
};

struct MbDeviceDescriptor {
    bool sio_available = false;
    MbSioDeviceDescriptor sio;
};

struct MbSioWritePolicy {
    bool writes_enabled = false;
    bool restore_on_exit = true;
    std::vector<std::uint32_t> blocked_channels;
};

enum class MbSioStatus : std::int32_t {
    ok = 0,
    error = -1,
    invalid_arg = -2,
    not_supported = -4,
    no_device = -5,
    access_denied = -6,
};

inline const char* mb_sio_status_string(MbSioStatus status) {
    switch (status) {
        case MbSioStatus::ok: return "ok";
        case MbSioStatus::error: return "error";
        case MbSioStatus::invalid_arg: return "invalid_arg";
        case MbSioStatus::not_supported: return "not_supported";
        case MbSioStatus::no_device: return "no_device";
        case MbSioStatus::access_denied: return "access_denied";
        default: return "unknown";
    }
}

class MbSioController {
public:
    MbSioController();
    ~MbSioController();

    MbSioController(const MbSioController&) = delete;
    MbSioController& operator=(const MbSioController&) = delete;
    MbSioController(MbSioController&&) noexcept;
    MbSioController& operator=(MbSioController&&) noexcept;

    /* Initialize the SIO transport. Returns false on failure.
     * Thread-safe: lifecycle operations are serialized by an internal lock. */
    bool init(const MbSioWritePolicy& policy, std::string& out_warning);

    /* Shutdown and release all handles. Restores channels to saved baseline
     * state if restore_on_exit is enabled. */
    void shutdown();

    /* Enumerate hardware and return a device descriptor snapshot.
     * Increments the generation counter; previous descriptors become stale. */
    MbDeviceDescriptor discover();

    MbSioStatus read_fans(const MbDeviceDescriptor& dev,
                          std::vector<MbFanSnapshot>& out);

    MbSioStatus read_voltages(const MbDeviceDescriptor& dev,
                              std::vector<MbVoltageSnapshot>& out);

    MbSioStatus read_sio_temperatures(const MbDeviceDescriptor& dev,
                                      std::vector<MbSioTemperatureSnapshot>& out);

    /* Set a single channel to a fixed duty cycle (0.0-100.0%).
     * Rejects if writes are disabled, the channel is blocked, or the
     * descriptor generation is stale. */
    MbSioStatus set_fan_duty(const MbDeviceDescriptor& dev,
                             std::uint32_t channel,
                             double duty_percent);

    MbSioStatus restore_fan_auto(const MbDeviceDescriptor& dev,
                                 std::uint32_t channel);

    MbSioStatus restore_all_fans(const MbDeviceDescriptor& dev);

    MbSioStatus restore_saved_state(const MbDeviceDescriptor& dev,
                                    std::uint32_t channel,
                                    std::uint8_t duty_raw,
                                    std::uint8_t mode_raw);

    MbSioStatus read_raw_register(const MbDeviceDescriptor& dev,
                                  std::uint16_t reg,
                                  std::uint8_t* out_value);

    MbSioStatus read_raw_block(const MbDeviceDescriptor& dev,
                               std::uint16_t base_reg,
                               std::uint8_t* out_values,
                               std::uint32_t count);

    bool writes_enabled() const;
    bool channel_blocked(std::uint32_t channel) const;
    const MbSioWritePolicy& write_policy() const;

private:
    std::unique_ptr<MbSioControllerImpl> impl_;
};

#endif /* SVG_MB_SIO_H */
