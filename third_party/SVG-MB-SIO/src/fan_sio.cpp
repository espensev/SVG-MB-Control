#include "fan_sio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

namespace mb::hw {

namespace {

constexpr std::uint16_t kNct6701PwmWrite[Nct6701Controller::kFanChannelCount] = {
    0x109u, 0x209u, 0x309u, 0x809u, 0x909u, 0xA09u, 0xB09u
};
constexpr std::uint16_t kNct6701PwmRead[Nct6701Controller::kFanChannelCount] = {
    0x001u, 0x003u, 0x011u, 0x013u, 0x015u, 0x017u, 0x029u
};
constexpr std::uint16_t kNct6701FanMode[Nct6701Controller::kFanChannelCount] = {
    0x102u, 0x202u, 0x302u, 0x802u, 0x902u, 0xA02u, 0xB02u
};
constexpr std::uint16_t kNct6701FanCount[Nct6701Controller::kFanChannelCount] = {
    0x4B0u, 0x4B2u, 0x4B4u, 0x4B6u, 0x4B8u, 0x4BAu, 0x4CCu
};
constexpr std::uint16_t kNct6701CountMin = 0x15u;
constexpr std::uint16_t kNct6701CountMax = 0x1FFFu;
constexpr double kNct6701RpmDivisor = 1350000.0;
constexpr std::array<const char*, Nct6701Controller::kFanChannelCount> kNct6701Labels = {
    "Channel0", "Channel1", "Channel2", "Channel3", "Channel4", "Channel5", "Channel6"
};

struct VoltageChannelDef {
    const char* label;
    std::uint16_t reg;
    double scale;
};

constexpr VoltageChannelDef kNct6701Voltages[Nct6701Controller::kVoltageChannelCount] = {
    {"VCore",           0x480u, 1.0},
    {"Voltage #2",      0x481u, 1.0},
    {"AVCC",            0x482u, 2.0},
    {"+3.3V",           0x483u, 2.0},
    {"Voltage #5",      0x484u, 1.0},
    {"Voltage #6",      0x485u, 1.0},
    {"Voltage #7",      0x486u, 1.0},
    {"+3V Standby",     0x487u, 2.0},
    {"VBat",            0x488u, 2.0},
    {"CPU Termination", 0x489u, 2.0},
    {"Voltage #11",     0x48Au, 2.0},
    {"Voltage #12",     0x48Bu, 1.0},
    {"Voltage #13",     0x48Cu, 1.0},
    {"Voltage #14",     0x48Du, 1.0},
    {"Voltage #15",     0x48Eu, 1.0},
    {"Voltage #16",     0x48Fu, 1.0},
};

struct TemperatureSourceDef {
    const char* label;
    std::uint16_t main_reg;
    std::uint16_t half_reg;
    std::int8_t half_bit;
};

constexpr TemperatureSourceDef kNct6701Temperatures[Nct6701Controller::kTemperatureSourceCount] = {
    {"PECI 0",        0x073u, 0x074u,  7},
    {"CPUTIN",        0x075u, 0x076u,  7},
    {"SYSTIN",        0x077u, 0x078u,  7},
    {"AUXTIN0",       0x079u, 0x07Au,  7},
    {"AUXTIN1",       0x07Bu, 0x07Cu,  7},
    {"AUXTIN2",       0x07Du, 0x07Eu,  7},
    {"AUXTIN3",       0x4A0u, 0x49Eu,  6},
    {"AUXTIN4",       0x027u, 0x000u, -1},
    {"PECI 1",        0x672u, 0x000u, -1},
    {"PCH CPU MAX",   0x674u, 0x000u, -1},
    {"PCH Chip",      0x676u, 0x000u, -1},
    {"PCH CPU",       0x678u, 0x000u, -1},
    {"PCH MCH",       0x67Au, 0x000u, -1},
    {"Agent0 DIMM0",  0x405u, 0x000u, -1},
    {"Agent0 DIMM1",  0x406u, 0x000u, -1},
    {"Agent1 DIMM0",  0x407u, 0x000u, -1},
    {"Agent1 DIMM1",  0x408u, 0x000u, -1},
    {"SMBUSMASTER 0", 0x150u, 0x151u,  7},
    {"SMBUSMASTER 1", 0x670u, 0x000u, -1},
    {"BYTE TEMP0",    0x419u, 0x000u, -1},
    {"BYTE TEMP1",    0x41Au, 0x000u, -1},
    {"PECI 0 Cal",    0x4F4u, 0x000u, -1},
    {"PECI 1 Cal",    0x4F5u, 0x000u, -1},
};

constexpr std::uint16_t kVbatMonitorControlReg = 0x005Du;

constexpr std::uint16_t kNct6701ChipId = 0xD806u;
constexpr std::uint8_t kSioLdnHwm = 0x0Bu;
constexpr std::uint8_t kSioEnterKey = 0x87u;
constexpr std::uint8_t kSioExitKey = 0xAAu;
constexpr wchar_t kIsaMutexName[] = L"Global\\Access_ISABUS.HTP.Method";
constexpr DWORD kIsaMutexTimeoutMs = 100u;
constexpr const char kPawnIoDevicePath[] = "\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr std::uint32_t kPawnIoLoadBinary = (41394u << 16) | (0x821u << 2);
constexpr std::uint32_t kPawnIoExecuteFn = (41394u << 16) | (0x841u << 2);
constexpr std::size_t kPawnIoFnNameLength = 32u;

std::filesystem::path executable_directory() {
    std::array<char, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(std::string(buffer.data(), buffer.data() + length)).parent_path();
}

void add_unique_path(std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return;
    }

    const std::filesystem::path normalized = candidate.lexically_normal();
    if (std::find(paths.begin(), paths.end(), normalized) == paths.end()) {
        paths.push_back(normalized);
    }
}

void add_root_and_parents(std::vector<std::filesystem::path>& paths,
                          std::filesystem::path root,
                          std::size_t max_depth) {
    for (std::size_t depth = 0; depth < max_depth && !root.empty(); ++depth) {
        add_unique_path(paths, root);
        const std::filesystem::path parent = root.parent_path();
        if (parent == root) {
            break;
        }
        root = parent;
    }
}

const char* resolve_pawnio_lpc_bin_path(char* out_path, std::size_t out_path_size) {
    if (out_path == nullptr || out_path_size == 0u) {
        return nullptr;
    }
    out_path[0] = '\0';

    const DWORD env_length = GetEnvironmentVariableA("SVG_MB_PAWNIO_LPC_BIN",
                                                     out_path,
                                                     static_cast<DWORD>(out_path_size));
    if (env_length > 0u && env_length < out_path_size) {
        const DWORD attrs = GetFileAttributesA(out_path);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            return out_path;
        }
    }

    std::vector<std::filesystem::path> search_roots;
    add_root_and_parents(search_roots, executable_directory(), 6u);

    std::error_code cwd_ec;
    add_root_and_parents(search_roots, std::filesystem::current_path(cwd_ec), 6u);

    for (const auto& root : search_roots) {
        for (const auto& candidate : std::array<std::filesystem::path, 3>{
                 root / "resources" / "pawnio" / "LpcIO.bin",
                 root / "release" / "resources" / "pawnio" / "LpcIO.bin",
                 root / "dist" / "resources" / "pawnio" / "LpcIO.bin",
             }) {
            std::error_code exists_ec;
            if (std::filesystem::exists(candidate, exists_ec) &&
                !std::filesystem::is_directory(candidate, exists_ec)) {
                const std::string candidate_text = candidate.string();
                std::snprintf(out_path, out_path_size, "%s", candidate_text.c_str());
                return out_path;
            }
        }
    }

    return nullptr;
}

Status load_pawnio_binary(HANDLE handle, const char* bin_path) {
    if (handle == nullptr || bin_path == nullptr || bin_path[0] == '\0') {
        return Status::invalid_arg;
    }

    HANDLE file_handle = CreateFileA(bin_path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return status_from_win32_error(GetLastError());
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_handle, &file_size) || file_size.QuadPart <= 0 ||
        file_size.QuadPart > 1024 * 1024) {
        CloseHandle(file_handle);
        return Status::error;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(file_size.QuadPart));
    DWORD bytes_read = 0;
    if (!ReadFile(file_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) ||
        bytes_read != buffer.size()) {
        CloseHandle(file_handle);
        return Status::error;
    }
    CloseHandle(file_handle);

    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(handle,
                                    kPawnIoLoadBinary,
                                    buffer.data(),
                                    bytes_read,
                                    nullptr,
                                    0,
                                    &bytes_returned,
                                    nullptr);
    return ok ? Status::ok : status_from_win32_error(GetLastError());
}

HANDLE open_or_create_isa_mutex() {
    HANDLE handle = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, kIsaMutexName);
    if (handle != nullptr) {
        return handle;
    }

    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND) {
        return nullptr;
    }

    return CreateMutexW(nullptr, FALSE, kIsaMutexName);
}

}  // namespace

const char* sio_transport_kind_string(SioTransportKind kind) {
    switch (kind) {
        case SioTransportKind::pawnio_lpc:
            return "pawnio_lpc";
        default:
            return "none";
    }
}

const char* sio_rpm_interpretation_string() {
    return "count_register_13bit_1350000_div";
}

SioPortTransport::~SioPortTransport() {
    close();
}

Status SioPortTransport::open(std::string* warning_text) {
    close();
    return open_pawnio_lpc(warning_text);
}

Status SioPortTransport::open_pawnio_lpc(std::string* warning_text) {
    char bin_path[MAX_PATH] = {};
    const char* resolved_bin = resolve_pawnio_lpc_bin_path(bin_path, sizeof(bin_path));
    if (resolved_bin == nullptr) {
        if (warning_text != nullptr) {
            *warning_text =
                "LpcIO.bin was not found. Set SVG_MB_PAWNIO_LPC_BIN or keep "
                "a packaged resources\\pawnio copy available.";
        }
        return Status::not_supported;
    }

    HANDLE handle = CreateFileA(kPawnIoDevicePath,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (warning_text != nullptr) {
            *warning_text = "PawnIO device is not available for Super I/O LPC access.";
        }
        return status_from_win32_error(GetLastError());
    }

    const Status load_status = load_pawnio_binary(handle, resolved_bin);
    if (load_status != Status::ok) {
        CloseHandle(handle);
        if (warning_text != nullptr) {
            *warning_text = "Failed to load LpcIO.bin into PawnIO.";
        }
        return load_status;
    }

    handle_ = handle;
    kind_ = SioTransportKind::pawnio_lpc;
    artifact_path_ = resolved_bin;
    if (warning_text != nullptr) {
        warning_text->clear();
    }
    return Status::ok;
}

Status SioPortTransport::execute_fn(const char* fn_name,
                                    const std::int64_t* inputs,
                                    std::size_t input_count,
                                    std::int64_t* outputs,
                                    std::size_t output_count) const {
    if (!is_open() || fn_name == nullptr || (output_count > 0u && outputs == nullptr) || input_count > 4u) {
        return Status::invalid_arg;
    }

    std::array<std::uint8_t, kPawnIoFnNameLength + (4u * sizeof(std::int64_t))> in_buffer{};
    std::array<std::uint8_t, 4u * sizeof(std::int64_t)> out_buffer{};
    std::snprintf(reinterpret_cast<char*>(in_buffer.data()), kPawnIoFnNameLength, "%s", fn_name);

    for (std::size_t index = 0; index < input_count; ++index) {
        std::memcpy(in_buffer.data() + kPawnIoFnNameLength + (index * sizeof(std::int64_t)),
                    &inputs[index],
                    sizeof(std::int64_t));
    }

    const DWORD in_size = static_cast<DWORD>(kPawnIoFnNameLength + (input_count * sizeof(std::int64_t)));
    const DWORD out_size = static_cast<DWORD>(output_count * sizeof(std::int64_t));
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(handle_,
                                    kPawnIoExecuteFn,
                                    in_buffer.data(),
                                    in_size,
                                    out_buffer.data(),
                                    out_size,
                                    &bytes_returned,
                                    nullptr);
    if (!ok) {
        return status_from_win32_error(GetLastError());
    }
    if (bytes_returned < out_size) {
        return Status::error;
    }

    for (std::size_t index = 0; index < output_count; ++index) {
        std::memcpy(&outputs[index], out_buffer.data() + (index * sizeof(std::int64_t)),
                    sizeof(std::int64_t));
    }

    return Status::ok;
}

Status SioPortTransport::select_slot(std::uint32_t slot) const {
    const std::int64_t input = static_cast<std::int64_t>(slot);
    return execute_fn("ioctl_select_slot", &input, 1u, nullptr, 0u);
}

Status SioPortTransport::find_bars() const {
    return execute_fn("ioctl_find_bars", nullptr, 0u, nullptr, 0u);
}

Status SioPortTransport::read_superio_byte(std::uint8_t reg, std::uint8_t* out_value) const {
    if (out_value == nullptr) {
        return Status::invalid_arg;
    }
    const std::int64_t input = static_cast<std::int64_t>(reg);
    std::int64_t output = 0;
    const Status status = execute_fn("ioctl_superio_inb", &input, 1u, &output, 1u);
    if (status == Status::ok) {
        *out_value = static_cast<std::uint8_t>(output);
    }
    return status;
}

Status SioPortTransport::read_superio_word(std::uint8_t reg, std::uint16_t* out_value) const {
    if (out_value == nullptr) {
        return Status::invalid_arg;
    }
    const std::int64_t input = static_cast<std::int64_t>(reg);
    std::int64_t output = 0;
    const Status status = execute_fn("ioctl_superio_inw", &input, 1u, &output, 1u);
    if (status == Status::ok) {
        *out_value = static_cast<std::uint16_t>(output);
    }
    return status;
}

Status SioPortTransport::write_superio_byte(std::uint8_t reg, std::uint8_t value) const {
    const std::array<std::int64_t, 2> inputs = {
        static_cast<std::int64_t>(reg),
        static_cast<std::int64_t>(value),
    };
    return execute_fn("ioctl_superio_outb", inputs.data(), inputs.size(), nullptr, 0u);
}

void SioPortTransport::close() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
    }
    handle_ = nullptr;
    kind_ = SioTransportKind::none;
    artifact_path_.clear();
}

bool SioPortTransport::is_open() const {
    return handle_ != nullptr;
}

Status SioPortTransport::read_port(std::uint16_t port, std::uint8_t* out_value) const {
    if (!is_open() || out_value == nullptr) {
        return Status::invalid_arg;
    }

    if (kind_ == SioTransportKind::pawnio_lpc) {
        const std::int64_t input = static_cast<std::int64_t>(port);
        std::int64_t output = 0;
        const Status status = execute_fn("ioctl_pio_inb", &input, 1u, &output, 1u);
        if (status == Status::ok) {
            *out_value = static_cast<std::uint8_t>(output);
        }
        return status;
    }

    return Status::not_supported;
}

Status SioPortTransport::write_port(std::uint16_t port, std::uint8_t value) const {
    if (!is_open()) {
        return Status::invalid_arg;
    }

    if (kind_ == SioTransportKind::pawnio_lpc) {
        const std::array<std::int64_t, 2> inputs = {
            static_cast<std::int64_t>(port),
            static_cast<std::int64_t>(value),
        };
        return execute_fn("ioctl_pio_outb", inputs.data(), inputs.size(), nullptr, 0u);
    }

    return Status::not_supported;
}

const std::string& SioPortTransport::artifact_path() const {
    return artifact_path_;
}

SioTransportKind SioPortTransport::kind() const {
    return kind_;
}

Nct6701Controller::~Nct6701Controller() {
    close();
}

Status Nct6701Controller::open(std::string* warning_text) {
    close();

    Status status = transport_.open(warning_text);
    if (status != Status::ok) {
        return status;
    }

    mutex_handle_ = open_or_create_isa_mutex();
    if (mutex_handle_ == nullptr) {
        const DWORD mutex_error = GetLastError();
        if (warning_text != nullptr) {
            *warning_text = "Could not open the Global\\Access_ISABUS.HTP.Method mutex.";
        }
        close();
        return status_from_win32_error(mutex_error);
    }

    status = detect_chip();
    if (status != Status::ok) {
        close();
        if (warning_text != nullptr && warning_text->empty()) {
            *warning_text = "No supported NCT6701D Super I/O device was detected.";
        }
        return status;
    }

    if (chip_id_ != kNct6701ChipId) {
        if (warning_text != nullptr) {
            char buffer[128];
            std::snprintf(buffer,
                          sizeof(buffer),
                          "Detected Nuvoton chip 0x%04X, but this build only supports the NCT6701D map.",
                          chip_id_);
            *warning_text = buffer;
        }
        close();
        return Status::not_supported;
    }

    if (warning_text != nullptr) {
        warning_text->clear();
    }

    // Enable VBat monitoring (register 0x005D bit 0) so voltage index 8
    // returns the CMOS battery voltage instead of a stale ADC value.
    {
        const Status vbat_lock = lock_mutex();
        if (vbat_lock == Status::ok) {
            std::uint8_t vbat_ctrl = 0;
            if (read_byte_locked(kVbatMonitorControlReg, &vbat_ctrl) == Status::ok) {
                if ((vbat_ctrl & 0x01u) == 0) {
                    write_byte_locked(kVbatMonitorControlReg,
                                      static_cast<std::uint8_t>(vbat_ctrl | 0x01u));
                }
            }
            unlock_mutex();
        }
    }

    return Status::ok;
}

void Nct6701Controller::close() {
    restore_all();
    if (mutex_handle_ != nullptr) {
        CloseHandle(mutex_handle_);
        mutex_handle_ = nullptr;
    }
    transport_.close();
    chip_id_ = 0u;
    chip_revision_ = 0u;
    hwm_base_ = 0u;
    index_port_ = 0u;
    std::memset(saved_modes_, 0, sizeof(saved_modes_));
    std::memset(saved_duty_raw_, 0, sizeof(saved_duty_raw_));
    std::memset(has_saved_, 0, sizeof(has_saved_));
}

bool Nct6701Controller::is_open() const {
    return transport_.is_open() && hwm_base_ != 0u;
}

Status Nct6701Controller::detect_chip() {
    auto probe_index_port = [&](std::uint16_t index_port) -> Status {
        const Status lock_status = lock_mutex();
        if (lock_status != Status::ok) {
            return lock_status;
        }

        const std::uint32_t slot = (index_port == 0x2Eu) ? 0u : 1u;
        Status status = transport_.select_slot(slot);
        if (status == Status::ok) {
            status = transport_.write_port(index_port, kSioEnterKey);
        }
        if (status == Status::ok) {
            status = transport_.write_port(index_port, kSioEnterKey);
        }

        std::uint8_t id_hi = 0u;
        std::uint8_t id_lo = 0u;
        std::uint8_t revision = 0u;
        std::uint16_t hwm_base = 0u;

        if (status == Status::ok) {
            status = transport_.read_superio_byte(0x20u, &id_hi);
        }
        if (status == Status::ok) {
            status = transport_.read_superio_byte(0x21u, &id_lo);
        }
        if (status == Status::ok) {
            status = transport_.read_superio_byte(0x22u, &revision);
        }

        const std::uint16_t chip_id = static_cast<std::uint16_t>((id_hi << 8) | id_lo);
        if (status == Status::ok &&
            chip_id != 0x0000u &&
            chip_id != 0xFFFFu &&
            (chip_id & 0xF000u) == 0xD000u) {
            status = transport_.find_bars();
            if (status == Status::ok) {
                status = transport_.write_superio_byte(0x07u, kSioLdnHwm);
            }
            if (status == Status::ok) {
                status = transport_.read_superio_word(0x60u, &hwm_base);
            }
        }

        transport_.write_port(index_port, kSioExitKey);
        unlock_mutex();

        if (status != Status::ok) {
            return status;
        }

        if (chip_id == 0x0000u || chip_id == 0xFFFFu || (chip_id & 0xF000u) != 0xD000u) {
            return Status::no_device;
        }
        if (hwm_base == 0x0000u || hwm_base == 0xFFFFu || (hwm_base & 0x000Fu) != 0u) {
            return Status::no_device;
        }

        chip_id_ = chip_id;
        chip_revision_ = revision;
        hwm_base_ = hwm_base;
        index_port_ = index_port;
        return Status::ok;
    };

    Status status = probe_index_port(0x2Eu);
    if (status == Status::ok) {
        return status;
    }
    if (status != Status::no_device) {
        return status;
    }
    return probe_index_port(0x4Eu);
}

Status Nct6701Controller::read_state(std::uint32_t channel, SioFanState* out_state) {
    if (!is_open() || out_state == nullptr || channel >= kFanChannelCount) {
        return Status::invalid_arg;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    std::uint8_t count_hi_raw = 0u;
    std::uint8_t count_lo_raw = 0u;
    std::uint8_t duty_raw = 0u;
    std::uint8_t mode_raw = 0u;
    Status status = read_byte_locked(kNct6701FanCount[channel], &count_hi_raw);
    if (status == Status::ok) {
        status = read_byte_locked(static_cast<std::uint16_t>(kNct6701FanCount[channel] + 1u), &count_lo_raw);
    }
    if (status == Status::ok) {
        status = read_byte_locked(kNct6701PwmRead[channel], &duty_raw);
    }
    if (status == Status::ok) {
        status = read_byte_locked(kNct6701FanMode[channel], &mode_raw);
    }
    unlock_mutex();

    if (status != Status::ok) {
        return status;
    }

    const std::uint16_t count = static_cast<std::uint16_t>((count_hi_raw << 5) | (count_lo_raw & 0x1Fu));
    const bool tach_valid = (count >= kNct6701CountMin && count <= kNct6701CountMax);
    out_state->rpm = tach_valid ? static_cast<std::uint16_t>(kNct6701RpmDivisor / count) : 0u;
    out_state->tach_raw = count;
    out_state->tach_hi_raw = count_hi_raw;
    out_state->tach_lo_raw = count_lo_raw;
    out_state->tach_valid = tach_valid;
    out_state->duty_raw = duty_raw;
    out_state->mode_raw = mode_raw;
    out_state->manual_override = has_saved_[channel];
    return Status::ok;
}

Status Nct6701Controller::set_duty_percent(std::uint32_t channel,
                                           double duty_percent,
                                           std::uint8_t* out_raw) {
    if (!is_open() || channel >= kFanChannelCount) {
        return Status::invalid_arg;
    }

    const double clamped = std::clamp(duty_percent, 0.0, 100.0);
    const std::uint8_t duty_raw = static_cast<std::uint8_t>(std::lround(clamped * 255.0 / 100.0));

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    if (!has_saved_[channel]) {
        std::uint8_t saved_mode = 0u;
        std::uint8_t saved_duty_raw = 0u;
        Status status = read_byte_locked(kNct6701FanMode[channel], &saved_mode);
        if (status == Status::ok) {
            status = read_byte_locked(kNct6701PwmRead[channel], &saved_duty_raw);
        }
        if (status != Status::ok) {
            unlock_mutex();
            return status;
        }
        saved_modes_[channel] = saved_mode;
        saved_duty_raw_[channel] = saved_duty_raw;
        has_saved_[channel] = true;
    }

    std::uint8_t current_mode = 0u;
    Status status = read_byte_locked(kNct6701FanMode[channel], &current_mode);
    if (status == Status::ok) {
        const std::uint8_t control_mode = static_cast<std::uint8_t>(current_mode & 0x0Fu);
        status = write_byte_locked(kNct6701FanMode[channel], control_mode);
    }
    if (status == Status::ok) {
        status = write_byte_locked(kNct6701PwmWrite[channel], duty_raw);
    }
    if (status == Status::ok) {
        unlock_mutex();
        if (out_raw != nullptr) {
            *out_raw = duty_raw;
        }
        return Status::ok;
    }
    unlock_mutex();
    return status;
}

Status Nct6701Controller::restore_auto(std::uint32_t channel) {
    if (!is_open() || channel >= kFanChannelCount) {
        return Status::invalid_arg;
    }
    if (!has_saved_[channel]) {
        return Status::ok;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    // Sanitize saved mode: mode_raw == 0 means "manual/fixed" which is
    // never a valid motherboard-auto baseline on NCT6701D. It indicates
    // the baseline was captured from a prior write-session that never
    // completed its restore (e.g., hard-killed before exit handler
    // fired). Fall back to the known motherboard SmartFan auto mode.
    constexpr std::uint8_t kNct6701AutoModeFallback = 0x40u;
    const std::uint8_t restore_mode = (saved_modes_[channel] == 0u)
                                          ? kNct6701AutoModeFallback
                                          : saved_modes_[channel];

    Status status = write_byte_locked(kNct6701PwmWrite[channel], saved_duty_raw_[channel]);
    if (status == Status::ok) {
        status = write_byte_locked(kNct6701FanMode[channel], restore_mode);
    }
    if (status == Status::ok) {
        has_saved_[channel] = false;
        saved_duty_raw_[channel] = 0u;
        saved_modes_[channel] = 0u;
    }
    unlock_mutex();
    return status;
}

Status Nct6701Controller::restore_saved_state(std::uint32_t channel,
                                              std::uint8_t duty_raw,
                                              std::uint8_t mode_raw) {
    if (!is_open() || channel >= kFanChannelCount) {
        return Status::invalid_arg;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    // Same sanitization as restore_auto: explicit mode_raw == 0 is never
    // a valid motherboard-auto baseline. See comment in restore_auto().
    constexpr std::uint8_t kNct6701AutoModeFallback = 0x40u;
    const std::uint8_t effective_mode = (mode_raw == 0u)
                                            ? kNct6701AutoModeFallback
                                            : mode_raw;

    Status status = write_byte_locked(kNct6701PwmWrite[channel], duty_raw);
    if (status == Status::ok) {
        status = write_byte_locked(kNct6701FanMode[channel], effective_mode);
    }
    if (status == Status::ok) {
        has_saved_[channel] = false;
        saved_duty_raw_[channel] = 0u;
        saved_modes_[channel] = 0u;
    }
    unlock_mutex();
    return status;
}

void Nct6701Controller::restore_all() {
    if (!is_open()) {
        return;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return;
    }

    for (std::uint32_t channel = 0; channel < kFanChannelCount; ++channel) {
        if (!has_saved_[channel]) {
            continue;
        }
        // Same sanitization as restore_auto.
        constexpr std::uint8_t kNct6701AutoModeFallback = 0x40u;
        const std::uint8_t restore_mode = (saved_modes_[channel] == 0u)
                                              ? kNct6701AutoModeFallback
                                              : saved_modes_[channel];
        Status status = write_byte_locked(kNct6701PwmWrite[channel], saved_duty_raw_[channel]);
        if (status == Status::ok) {
            status = write_byte_locked(kNct6701FanMode[channel], restore_mode);
        }
        if (status == Status::ok) {
            has_saved_[channel] = false;
            saved_duty_raw_[channel] = 0u;
            saved_modes_[channel] = 0u;
        }
    }

    unlock_mutex();
}

std::uint32_t Nct6701Controller::fan_count() const {
    return kFanChannelCount;
}

const char* Nct6701Controller::channel_label(std::uint32_t channel) const {
    if (channel >= kFanChannelCount) {
        return "unknown";
    }
    return kNct6701Labels[channel];
}

SioTransportKind Nct6701Controller::transport_kind() const {
    return transport_.kind();
}

const std::string& Nct6701Controller::transport_artifact_path() const {
    return transport_.artifact_path();
}

std::uint16_t Nct6701Controller::chip_id() const {
    return chip_id_;
}

std::uint8_t Nct6701Controller::chip_revision() const {
    return chip_revision_;
}

std::uint16_t Nct6701Controller::hwm_base() const {
    return hwm_base_;
}

std::uint16_t Nct6701Controller::index_port() const {
    return index_port_;
}

std::uint32_t Nct6701Controller::voltage_count() const {
    return kVoltageChannelCount;
}

std::uint32_t Nct6701Controller::temperature_count() const {
    return kTemperatureSourceCount;
}

const char* Nct6701Controller::voltage_label(std::uint32_t index) {
    if (index >= kVoltageChannelCount) {
        return "unknown";
    }
    return kNct6701Voltages[index].label;
}

const char* Nct6701Controller::temperature_label(std::uint32_t index) {
    if (index >= kTemperatureSourceCount) {
        return "unknown";
    }
    return kNct6701Temperatures[index].label;
}

Status Nct6701Controller::read_all_voltages(SioVoltageState* out_states) {
    if (!is_open() || out_states == nullptr) {
        return Status::invalid_arg;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    for (std::uint32_t i = 0; i < kVoltageChannelCount; ++i) {
        std::uint8_t raw = 0;
        const Status status = read_byte_locked(kNct6701Voltages[i].reg, &raw);
        if (status != Status::ok) {
            unlock_mutex();
            return status;
        }
        out_states[i].raw = raw;
        out_states[i].voltage = static_cast<double>(raw) * 0.008 * kNct6701Voltages[i].scale;
    }

    unlock_mutex();
    return Status::ok;
}

Status Nct6701Controller::read_all_temperatures(SioTemperatureState* out_states) {
    if (!is_open() || out_states == nullptr) {
        return Status::invalid_arg;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    for (std::uint32_t i = 0; i < kTemperatureSourceCount; ++i) {
        std::uint8_t main_raw = 0;
        Status status = read_byte_locked(kNct6701Temperatures[i].main_reg, &main_raw);
        if (status != Status::ok) {
            unlock_mutex();
            return status;
        }

        int value = static_cast<int>(static_cast<std::int8_t>(main_raw)) << 1;
        std::uint8_t half_raw = 0;
        if (kNct6701Temperatures[i].half_bit >= 0 && kNct6701Temperatures[i].half_reg != 0) {
            status = read_byte_locked(kNct6701Temperatures[i].half_reg, &half_raw);
            if (status != Status::ok) {
                unlock_mutex();
                return status;
            }
            value |= (half_raw >> kNct6701Temperatures[i].half_bit) & 0x1;
        }

        const double temp_c = 0.5 * value;
        out_states[i].raw = main_raw;
        out_states[i].half_raw = half_raw;
        out_states[i].temperature_c = temp_c;
        out_states[i].valid = (main_raw != 0x00u && main_raw != 0x80u &&
                               temp_c >= -55.0 && temp_c <= 125.0);
    }

    unlock_mutex();
    return Status::ok;
}

Status Nct6701Controller::read_raw_register(std::uint16_t reg, std::uint8_t* out_value) {
    if (!is_open() || out_value == nullptr) {
        return Status::invalid_arg;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    const Status status = read_byte_locked(reg, out_value);
    unlock_mutex();
    return status;
}

Status Nct6701Controller::read_raw_block(std::uint16_t base_reg,
                                         std::uint8_t* out_values,
                                         std::uint32_t count) {
    if (!is_open() || (count > 0u && out_values == nullptr)) {
        return Status::invalid_arg;
    }

    const Status lock_status = lock_mutex();
    if (lock_status != Status::ok) {
        return lock_status;
    }

    for (std::uint32_t offset = 0; offset < count; ++offset) {
        const Status status = read_byte_locked(static_cast<std::uint16_t>(base_reg + offset),
                                               &out_values[offset]);
        if (status != Status::ok) {
            unlock_mutex();
            return status;
        }
    }

    unlock_mutex();
    return Status::ok;
}

Status Nct6701Controller::read_byte_locked(std::uint16_t reg, std::uint8_t* out_value) const {
    if (out_value == nullptr || hwm_base_ == 0u) {
        return Status::invalid_arg;
    }

    const std::uint16_t addr_port = static_cast<std::uint16_t>(hwm_base_ + 5u);
    const std::uint16_t data_port = static_cast<std::uint16_t>(hwm_base_ + 6u);

    Status status = transport_.write_port(addr_port, 0x4Eu);
    if (status != Status::ok) {
        return status;
    }
    status = transport_.write_port(data_port, static_cast<std::uint8_t>(reg >> 8));
    if (status != Status::ok) {
        return status;
    }
    status = transport_.write_port(addr_port, static_cast<std::uint8_t>(reg & 0xFFu));
    if (status != Status::ok) {
        return status;
    }
    return transport_.read_port(data_port, out_value);
}

Status Nct6701Controller::write_byte_locked(std::uint16_t reg, std::uint8_t value) const {
    if (hwm_base_ == 0u) {
        return Status::invalid_arg;
    }

    const std::uint16_t addr_port = static_cast<std::uint16_t>(hwm_base_ + 5u);
    const std::uint16_t data_port = static_cast<std::uint16_t>(hwm_base_ + 6u);

    Status status = transport_.write_port(addr_port, 0x4Eu);
    if (status != Status::ok) {
        return status;
    }
    status = transport_.write_port(data_port, static_cast<std::uint8_t>(reg >> 8));
    if (status != Status::ok) {
        return status;
    }
    status = transport_.write_port(addr_port, static_cast<std::uint8_t>(reg & 0xFFu));
    if (status != Status::ok) {
        return status;
    }
    return transport_.write_port(data_port, value);
}

Status Nct6701Controller::read_u16_be_locked(std::uint16_t reg, std::uint16_t* out_value) const {
    if (out_value == nullptr) {
        return Status::invalid_arg;
    }

    std::uint8_t hi = 0u;
    std::uint8_t lo = 0u;
    Status status = read_byte_locked(reg, &hi);
    if (status == Status::ok) {
        status = read_byte_locked(static_cast<std::uint16_t>(reg + 1u), &lo);
    }
    if (status != Status::ok) {
        return status;
    }

    *out_value = static_cast<std::uint16_t>((hi << 8) | lo);
    return Status::ok;
}

Status Nct6701Controller::lock_mutex() const {
    if (mutex_handle_ == nullptr) {
        return Status::ok;
    }

    const DWORD result = WaitForSingleObject(mutex_handle_, kIsaMutexTimeoutMs);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        return Status::ok;
    }
    return Status::error;
}

void Nct6701Controller::unlock_mutex() const {
    if (mutex_handle_ != nullptr) {
        ReleaseMutex(mutex_handle_);
    }
}

}  // namespace mb::hw
