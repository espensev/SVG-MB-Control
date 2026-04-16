#pragma once

#include "driver_io.h"

#include <cstdint>
#include <string>

namespace mb::hw {

enum class SioTransportKind {
    none = 0,
    pawnio_lpc,
};

const char* sio_transport_kind_string(SioTransportKind kind);
const char* sio_rpm_interpretation_string();

class SioPortTransport {
public:
    ~SioPortTransport();

    Status open(std::string* warning_text);
    void close();
    bool is_open() const;

    Status read_port(std::uint16_t port, std::uint8_t* out_value) const;
    Status write_port(std::uint16_t port, std::uint8_t value) const;
    Status select_slot(std::uint32_t slot) const;
    Status find_bars() const;
    Status read_superio_byte(std::uint8_t reg, std::uint8_t* out_value) const;
    Status read_superio_word(std::uint8_t reg, std::uint16_t* out_value) const;
    Status write_superio_byte(std::uint8_t reg, std::uint8_t value) const;

    const std::string& artifact_path() const;
    SioTransportKind kind() const;

private:
    Status open_pawnio_lpc(std::string* warning_text);
    Status execute_fn(const char* fn_name,
                      const std::int64_t* inputs,
                      std::size_t input_count,
                      std::int64_t* outputs,
                      std::size_t output_count) const;

    HANDLE handle_ = nullptr;
    SioTransportKind kind_ = SioTransportKind::none;
    std::string artifact_path_;
};

struct SioFanState {
    std::uint16_t rpm = 0;
    std::uint16_t tach_raw = 0;
    std::uint8_t tach_hi_raw = 0;
    std::uint8_t tach_lo_raw = 0;
    bool tach_valid = false;
    std::uint8_t duty_raw = 0;
    std::uint8_t mode_raw = 0;
    bool manual_override = false;
};

struct SioVoltageState {
    double voltage = 0.0;
    std::uint8_t raw = 0;
};

struct SioTemperatureState {
    double temperature_c = 0.0;
    std::uint8_t raw = 0;
    std::uint8_t half_raw = 0;
    bool valid = false;
};

class Nct6701Controller {
public:
    static constexpr std::uint32_t kFanChannelCount = 7u;
    static constexpr std::uint32_t kVoltageChannelCount = 16u;
    static constexpr std::uint32_t kTemperatureSourceCount = 23u;

    ~Nct6701Controller();

    Status open(std::string* warning_text);
    void close();
    bool is_open() const;

    Status read_state(std::uint32_t channel, SioFanState* out_state);
    Status set_duty_percent(std::uint32_t channel,
                            double duty_percent,
                            std::uint8_t* out_raw = nullptr);
    Status restore_auto(std::uint32_t channel);
    Status restore_saved_state(std::uint32_t channel,
                               std::uint8_t duty_raw,
                               std::uint8_t mode_raw);
    void restore_all();

    std::uint32_t fan_count() const;
    const char* channel_label(std::uint32_t channel) const;

    Status read_all_voltages(SioVoltageState* out_states);
    Status read_all_temperatures(SioTemperatureState* out_states);
    std::uint32_t voltage_count() const;
    std::uint32_t temperature_count() const;
    static const char* voltage_label(std::uint32_t index);
    static const char* temperature_label(std::uint32_t index);

    Status read_raw_register(std::uint16_t reg, std::uint8_t* out_value);
    Status read_raw_block(std::uint16_t base_reg, std::uint8_t* out_values, std::uint32_t count);

    SioTransportKind transport_kind() const;
    const std::string& transport_artifact_path() const;
    std::uint16_t chip_id() const;
    std::uint8_t chip_revision() const;
    std::uint16_t hwm_base() const;
    std::uint16_t index_port() const;

private:
    Status detect_chip();
    Status read_byte_locked(std::uint16_t reg, std::uint8_t* out_value) const;
    Status write_byte_locked(std::uint16_t reg, std::uint8_t value) const;
    Status read_u16_be_locked(std::uint16_t reg, std::uint16_t* out_value) const;
    Status lock_mutex() const;
    void unlock_mutex() const;

    SioPortTransport transport_;
    HANDLE mutex_handle_ = nullptr;
    std::uint16_t chip_id_ = 0;
    std::uint8_t chip_revision_ = 0;
    std::uint16_t hwm_base_ = 0;
    std::uint16_t index_port_ = 0;
    std::uint8_t saved_modes_[kFanChannelCount] = {};
    std::uint8_t saved_duty_raw_[kFanChannelCount] = {};
    bool has_saved_[kFanChannelCount] = {};
};

}  // namespace mb::hw
