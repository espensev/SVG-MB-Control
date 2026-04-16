#include "svg_mb_sio/svg_mb_sio.h"

#include "fan_sio.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace {

MbSioStatus translate_status(mb::hw::Status status) {
    switch (status) {
        case mb::hw::Status::ok: return MbSioStatus::ok;
        case mb::hw::Status::invalid_arg: return MbSioStatus::invalid_arg;
        case mb::hw::Status::not_supported: return MbSioStatus::not_supported;
        case mb::hw::Status::no_device: return MbSioStatus::no_device;
        case mb::hw::Status::access_denied: return MbSioStatus::access_denied;
        default: return MbSioStatus::error;
    }
}

MbSioTransport translate_sio_transport(mb::hw::SioTransportKind kind) {
    switch (kind) {
        case mb::hw::SioTransportKind::pawnio_lpc: return MbSioTransport::pawnio_lpc;
        default: return MbSioTransport::none;
    }
}

}  // namespace

struct MbSioControllerImpl {
    mb::hw::Nct6701Controller sio;
    MbSioWritePolicy policy;
    bool initialized = false;
    bool sio_open = false;
    std::shared_mutex lifecycle_lock;
    std::mutex sio_write_lock;
    std::atomic<std::uint32_t> discover_gen{0};
};

MbSioController::MbSioController()
    : impl_(std::make_unique<MbSioControllerImpl>()) {}

MbSioController::~MbSioController() {
    if (impl_) {
        shutdown();
    }
}

MbSioController::MbSioController(MbSioController&&) noexcept = default;
MbSioController& MbSioController::operator=(MbSioController&&) noexcept = default;

bool MbSioController::init(const MbSioWritePolicy& policy, std::string& out_warning) {
    std::unique_lock lock(impl_->lifecycle_lock);

    if (impl_->initialized) {
        out_warning = "Already initialized.";
        return true;
    }

    impl_->policy = policy;
    out_warning.clear();

    const auto status = impl_->sio.open(&out_warning);
    impl_->sio_open = (status == mb::hw::Status::ok);
    impl_->initialized = impl_->sio_open;
    return impl_->initialized;
}

void MbSioController::shutdown() {
    std::unique_lock lock(impl_->lifecycle_lock);

    if (!impl_->initialized) {
        return;
    }

    if (impl_->sio_open) {
        if (impl_->policy.restore_on_exit) {
            impl_->sio.restore_all();
        }
        impl_->sio.close();
        impl_->sio_open = false;
    }

    impl_->initialized = false;
}

MbDeviceDescriptor MbSioController::discover() {
    std::unique_lock lock(impl_->lifecycle_lock);

    const std::uint32_t generation = ++impl_->discover_gen;
    MbDeviceDescriptor device;

    if (impl_->sio_open) {
        device.sio_available = true;
        device.sio.chip_id = impl_->sio.chip_id();
        device.sio.chip_revision = impl_->sio.chip_revision();
        device.sio.fan_count = impl_->sio.fan_count();
        device.sio.voltage_count = impl_->sio.voltage_count();
        device.sio.temp_count = impl_->sio.temperature_count();
        device.sio.transport = translate_sio_transport(impl_->sio.transport_kind());
        device.sio.transport_path = impl_->sio.transport_artifact_path();
        device.sio.hwm_base = impl_->sio.hwm_base();
        device.sio.index_port = impl_->sio.index_port();
        device.sio.generation = generation;
    }

    return device;
}

MbSioStatus MbSioController::read_fans(const MbDeviceDescriptor& dev,
                                       std::vector<MbFanSnapshot>& out) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    out.clear();
    out.resize(dev.sio.fan_count);

    for (std::uint32_t channel = 0; channel < dev.sio.fan_count; ++channel) {
        mb::hw::SioFanState state{};
        const auto status = impl_->sio.read_state(channel, &state);
        if (status != mb::hw::Status::ok) {
            return translate_status(status);
        }

        auto& snapshot = out[channel];
        snapshot.channel = channel;
        snapshot.rpm = state.rpm;
        snapshot.tach_raw = state.tach_raw;
        snapshot.duty_raw = state.duty_raw;
        snapshot.mode_raw = state.mode_raw;
        snapshot.duty_percent = static_cast<double>(state.duty_raw) * 100.0 / 255.0;
        snapshot.tach_valid = state.tach_valid;
        snapshot.manual_override = state.manual_override;
        snapshot.label = impl_->sio.channel_label(channel);
    }

    return MbSioStatus::ok;
}

MbSioStatus MbSioController::read_voltages(const MbDeviceDescriptor& dev,
                                           std::vector<MbVoltageSnapshot>& out) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    mb::hw::SioVoltageState states[mb::hw::Nct6701Controller::kVoltageChannelCount]{};
    const auto status = impl_->sio.read_all_voltages(states);
    if (status != mb::hw::Status::ok) {
        return translate_status(status);
    }

    out.clear();
    out.resize(dev.sio.voltage_count);
    for (std::uint32_t index = 0; index < dev.sio.voltage_count; ++index) {
        out[index].index = index;
        out[index].voltage_v = states[index].voltage;
        out[index].raw = states[index].raw;
        out[index].label = mb::hw::Nct6701Controller::voltage_label(index);
    }

    return MbSioStatus::ok;
}

MbSioStatus MbSioController::read_sio_temperatures(const MbDeviceDescriptor& dev,
                                                   std::vector<MbSioTemperatureSnapshot>& out) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    mb::hw::SioTemperatureState states[mb::hw::Nct6701Controller::kTemperatureSourceCount]{};
    const auto status = impl_->sio.read_all_temperatures(states);
    if (status != mb::hw::Status::ok) {
        return translate_status(status);
    }

    out.clear();
    out.resize(dev.sio.temp_count);
    for (std::uint32_t index = 0; index < dev.sio.temp_count; ++index) {
        out[index].index = index;
        out[index].temperature_c = states[index].temperature_c;
        out[index].raw = states[index].raw;
        out[index].half_raw = states[index].half_raw;
        out[index].valid = states[index].valid;
        out[index].label = mb::hw::Nct6701Controller::temperature_label(index);
    }

    return MbSioStatus::ok;
}

MbSioStatus MbSioController::set_fan_duty(const MbDeviceDescriptor& dev,
                                          std::uint32_t channel,
                                          double duty_percent) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }
    if (!impl_->policy.writes_enabled) {
        return MbSioStatus::not_supported;
    }
    if (channel_blocked(channel)) {
        return MbSioStatus::not_supported;
    }

    std::lock_guard write_lock(impl_->sio_write_lock);
    return translate_status(impl_->sio.set_duty_percent(channel, duty_percent));
}

MbSioStatus MbSioController::restore_fan_auto(const MbDeviceDescriptor& dev,
                                              std::uint32_t channel) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    std::lock_guard write_lock(impl_->sio_write_lock);
    return translate_status(impl_->sio.restore_auto(channel));
}

MbSioStatus MbSioController::restore_all_fans(const MbDeviceDescriptor& dev) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    std::lock_guard write_lock(impl_->sio_write_lock);
    impl_->sio.restore_all();
    return MbSioStatus::ok;
}

MbSioStatus MbSioController::restore_saved_state(const MbDeviceDescriptor& dev,
                                                 std::uint32_t channel,
                                                 std::uint8_t duty_raw,
                                                 std::uint8_t mode_raw) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    std::lock_guard write_lock(impl_->sio_write_lock);
    return translate_status(impl_->sio.restore_saved_state(channel, duty_raw, mode_raw));
}

MbSioStatus MbSioController::read_raw_register(const MbDeviceDescriptor& dev,
                                               std::uint16_t reg,
                                               std::uint8_t* out_value) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    return translate_status(impl_->sio.read_raw_register(reg, out_value));
}

MbSioStatus MbSioController::read_raw_block(const MbDeviceDescriptor& dev,
                                            std::uint16_t base_reg,
                                            std::uint8_t* out_values,
                                            std::uint32_t count) {
    std::shared_lock lock(impl_->lifecycle_lock);

    if (!impl_->sio_open || !dev.sio_available) {
        return MbSioStatus::no_device;
    }
    if (dev.sio.generation != impl_->discover_gen.load()) {
        return MbSioStatus::invalid_arg;
    }

    return translate_status(impl_->sio.read_raw_block(base_reg, out_values, count));
}

bool MbSioController::writes_enabled() const {
    return impl_->policy.writes_enabled;
}

bool MbSioController::channel_blocked(std::uint32_t channel) const {
    const auto& blocked = impl_->policy.blocked_channels;
    return std::find(blocked.begin(), blocked.end(), channel) != blocked.end();
}

const MbSioWritePolicy& MbSioController::write_policy() const {
    return impl_->policy;
}
