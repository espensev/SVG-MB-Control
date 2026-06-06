#include "amd_reader.h"

#include "windows_lean.h"

#include <intrin.h>

#include "amd_decode.h"
#include "env_util.h"
#include "pawnio_binary.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace svg_mb_control {

namespace {

enum class Status {
    ok = 0,
    error = -1,
    invalid_arg = -2,
    not_supported = -4,
    no_device = -5,
    access_denied = -6,
};

constexpr std::uint32_t kTctlTdieAddress = 0x00059800u;
constexpr std::uint32_t kMaxCcds = 8u;

// Constant per-CCD sensor labels. Indexing this table avoids rebuilding the
// label string ("CCD" + std::to_string(index+1) + " (Tdie)") on every CCD on
// every sample tick.
constexpr const char* kCcdSensorLabels[kMaxCcds] = {
    "CCD1 (Tdie)", "CCD2 (Tdie)", "CCD3 (Tdie)", "CCD4 (Tdie)",
    "CCD5 (Tdie)", "CCD6 (Tdie)", "CCD7 (Tdie)", "CCD8 (Tdie)",
};

constexpr const char kPawnIoDevicePath[] = "\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr std::uint32_t kPawnIoExecuteFn = (41394u << 16) | (0x841u << 2);
constexpr std::size_t kPawnIoFnNameLength = 32u;
constexpr DWORD kPciMutexTimeoutMs = 100u;
constexpr DWORD kPciMutexAccess = SYNCHRONIZE | MUTEX_MODIFY_STATE;
constexpr unsigned int kPawnIoOpenAttempts = 16u;
constexpr unsigned int kPawnIoOpenInitialDelayMs = 25u;
constexpr unsigned int kPawnIoOpenMaxDelayMs = 250u;

std::optional<double> TryParseDoubleEnv(const char* name) {
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr ||
        value[0] == '\0') {
        if (value != nullptr) {
            std::free(value);
        }
        return std::nullopt;
    }
    std::string text(value);
    std::free(value);
    try {
        return std::stod(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<double> ParseDoubleSequence(std::string_view text) {
    std::vector<double> values;
    std::size_t cursor = 0u;
    while (cursor < text.size()) {
        while (cursor < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[cursor])) != 0 ||
                text[cursor] == ',')) {
            ++cursor;
        }
        if (cursor >= text.size()) {
            break;
        }
        std::size_t end = cursor;
        while (end < text.size() && text[end] != ',') {
            ++end;
        }
        try {
            values.push_back(std::stod(std::string(text.substr(cursor, end - cursor))));
        } catch (const std::exception&) {
            values.clear();
            return values;
        }
        cursor = end;
    }
    return values;
}

Status StatusFromWin32Error(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_SERVICE_DOES_NOT_EXIST:
        case ERROR_DEV_NOT_EXIST:
            return Status::no_device;
        case ERROR_ACCESS_DENIED:
            return Status::access_denied;
        case ERROR_NOT_SUPPORTED:
            return Status::not_supported;
        default:
            return Status::error;
    }
}

const char* StatusString(Status status) {
    switch (status) {
        case Status::ok: return "ok";
        case Status::error: return "error";
        case Status::invalid_arg: return "invalid_arg";
        case Status::not_supported: return "not_supported";
        case Status::no_device: return "no_device";
        case Status::access_denied: return "access_denied";
        default: return "unknown";
    }
}

Status MapPawnIoStatus(PawnIoStatus status) {
    switch (status) {
        case PawnIoStatus::ok: return Status::ok;
        case PawnIoStatus::invalid_arg: return Status::invalid_arg;
        case PawnIoStatus::not_supported: return Status::not_supported;
        case PawnIoStatus::not_found: return Status::no_device;
        case PawnIoStatus::access_denied: return Status::access_denied;
        case PawnIoStatus::hash_mismatch: return Status::error;
        case PawnIoStatus::error:
        default: return Status::error;
    }
}

bool IsRetryablePawnIoOpenError(DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED:
        case ERROR_BUSY:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_FILE_NOT_FOUND:
        case ERROR_LOCK_VIOLATION:
        case ERROR_NOT_READY:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_SHARING_VIOLATION:
            return true;
        default:
            return false;
    }
}

bool IsRetryablePawnIoLoadStatus(PawnIoStatus status) {
    return status == PawnIoStatus::access_denied ||
           status == PawnIoStatus::error ||
           status == PawnIoStatus::not_found;
}

void SleepBeforePawnIoRetry(unsigned int attempt) {
    unsigned int delay_ms = kPawnIoOpenInitialDelayMs << attempt;
    if (delay_ms > kPawnIoOpenMaxDelayMs) {
        delay_ms = kPawnIoOpenMaxDelayMs;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

HANDLE OpenOrCreatePciMutex() {
    HANDLE handle = OpenMutexA(kPciMutexAccess, FALSE, "Global\\Access_PCI");
    if (handle != nullptr) {
        return handle;
    }
    return CreateMutexA(nullptr, FALSE, "Global\\Access_PCI");
}

class PciMutexLock {
public:
    explicit PciMutexLock(HANDLE handle) : handle_(handle) {
        if (handle_ == nullptr) {
            return;
        }
        const DWORD wait_result = WaitForSingleObject(handle_, kPciMutexTimeoutMs);
        acquired_ = wait_result == WAIT_OBJECT_0 ||
                    wait_result == WAIT_ABANDONED;
    }

    ~PciMutexLock() {
        if (acquired_) {
            ReleaseMutex(handle_);
        }
    }

    PciMutexLock(const PciMutexLock&) = delete;
    PciMutexLock& operator=(const PciMutexLock&) = delete;

    bool acquired() const {
        return acquired_;
    }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

Status ExecutePawnIo(HANDLE handle,
                     const char* fn_name,
                     const std::int64_t* inputs,
                     std::size_t input_count,
                     std::int64_t* outputs,
                     std::size_t output_count) {
    if (handle == nullptr || fn_name == nullptr || outputs == nullptr ||
        output_count == 0u || input_count > 4u) {
        return Status::invalid_arg;
    }

    std::array<std::uint8_t, kPawnIoFnNameLength + (8u * 4u)> in_buffer{};
    std::array<std::uint8_t, 8u * 4u> out_buffer{};
    std::snprintf(reinterpret_cast<char*>(in_buffer.data()),
                  kPawnIoFnNameLength, "%s", fn_name);

    for (std::size_t index = 0u; index < input_count; ++index) {
        std::memcpy(in_buffer.data() + kPawnIoFnNameLength +
                        (index * sizeof(std::int64_t)),
                    &inputs[index], sizeof(std::int64_t));
    }

    const DWORD in_size = static_cast<DWORD>(
        kPawnIoFnNameLength + (input_count * sizeof(std::int64_t)));
    const DWORD out_size =
        static_cast<DWORD>(output_count * sizeof(std::int64_t));
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(handle, kPawnIoExecuteFn, in_buffer.data(),
                                    in_size, out_buffer.data(), out_size,
                                    &bytes_returned, nullptr);
    if (!ok || bytes_returned < sizeof(std::int64_t)) {
        return Status::error;
    }

    for (std::size_t index = 0u;
         index < output_count &&
         ((index + 1u) * sizeof(std::int64_t)) <= bytes_returned;
         ++index) {
        std::memcpy(&outputs[index],
                    out_buffer.data() + (index * sizeof(std::int64_t)),
                    sizeof(std::int64_t));
    }
    return Status::ok;
}

bool DetectAmdCpu(std::string* out_name,
                  std::uint32_t* out_family,
                  std::uint32_t* out_model) {
    int cpu_info[4] = {};
    __cpuid(cpu_info, 0);

    char vendor[13] = {};
    reinterpret_cast<int*>(vendor)[0] = cpu_info[1];
    reinterpret_cast<int*>(vendor)[1] = cpu_info[3];
    reinterpret_cast<int*>(vendor)[2] = cpu_info[2];
    vendor[12] = '\0';
    if (std::strcmp(vendor, "AuthenticAMD") != 0) {
        return false;
    }

    __cpuid(cpu_info, 1);
    const std::uint32_t eax = static_cast<std::uint32_t>(cpu_info[0]);
    const std::uint32_t base_family = (eax >> 8) & 0xFu;
    const std::uint32_t base_model = (eax >> 4) & 0xFu;
    const std::uint32_t ext_family = (eax >> 20) & 0xFFu;
    const std::uint32_t ext_model = (eax >> 16) & 0xFu;

    std::uint32_t display_family = base_family;
    if (base_family == 0xFu) {
        display_family += ext_family;
    }

    std::uint32_t display_model = base_model;
    if (base_family == 0x6u || base_family == 0xFu) {
        display_model |= (ext_model << 4);
    }

    char brand[49] = {};
    __cpuid(cpu_info, 0x80000000);
    const int max_extended = cpu_info[0];
    if (max_extended >= 0x80000004) {
        __cpuid(reinterpret_cast<int*>(brand + 0), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
    }

    if (out_name != nullptr) {
        *out_name = (brand[0] == '\0') ? std::string("AMD CPU")
                                       : std::string(brand);
    }
    if (out_family != nullptr) {
        *out_family = display_family;
    }
    if (out_model != nullptr) {
        *out_model = display_model;
    }
    return true;
}

}  // namespace

struct AmdReader::Impl {
    bool sim_mode = false;
    bool initialized = false;
    std::string init_warning;

    HANDLE handle = nullptr;
    HANDLE mutex_handle = nullptr;
    std::string cpu_name;
    std::string transport_path;
    std::uint32_t cpu_family = 0u;
    std::uint32_t cpu_model = 0u;
    bool supports_ccd = false;
    std::uint32_t ccd_base = 0u;
    std::uint32_t ccd_count_hint = 0u;
    std::vector<double> sim_tctl_sequence;
    std::size_t sim_tctl_index = 0u;

    ~Impl() {
        Close();
    }

    void Close() {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        if (mutex_handle != nullptr) {
            CloseHandle(mutex_handle);
        }
        handle = nullptr;
        mutex_handle = nullptr;
        initialized = false;
        supports_ccd = false;
        ccd_base = 0u;
        ccd_count_hint = 0u;
        transport_path.clear();
        cpu_name.clear();
    }

    Status OpenReal(std::string* warning_text) {
        std::string detected_cpu_name;
        if (!DetectAmdCpu(&detected_cpu_name, &cpu_family, &cpu_model)) {
            if (warning_text != nullptr) {
                *warning_text = "Current CPU is not AuthenticAMD.";
            }
            return Status::not_supported;
        }

        const std::filesystem::path pawnio_bin =
            ResolvePawnIoBinaryPath(kPawnIoSpecAmdFamily17V1);
        if (pawnio_bin.empty()) {
            if (warning_text != nullptr) {
                *warning_text =
                    "AMDFamily17.bin was not found. Set "
                    "SVG_MB_CONTROL_PAWNIO_BIN or SVG_MB_PAWNIO_BIN, or keep "
                    "a packaged resources\\pawnio copy available.";
            }
            return Status::not_supported;
        }

        HANDLE pawnio_handle = INVALID_HANDLE_VALUE;
        std::string load_warning;
        DWORD last_open_error = ERROR_SUCCESS;
        PawnIoStatus last_load_status = PawnIoStatus::error;
        Status last_status = Status::error;
        bool failed_during_open = true;
        unsigned int attempts = 0u;
        for (unsigned int attempt = 0u; attempt < kPawnIoOpenAttempts;
             ++attempt) {
            attempts = attempt + 1u;
            pawnio_handle = CreateFileA(
                kPawnIoDevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                nullptr);
            if (pawnio_handle == INVALID_HANDLE_VALUE) {
                last_open_error = GetLastError();
                last_status = StatusFromWin32Error(last_open_error);
                failed_during_open = true;
                if (attempt + 1u >= kPawnIoOpenAttempts ||
                    !IsRetryablePawnIoOpenError(last_open_error)) {
                    break;
                }
                SleepBeforePawnIoRetry(attempt);
                continue;
            }

            load_warning.clear();
            last_load_status = LoadPawnIoBinary(
                pawnio_handle, kPawnIoSpecAmdFamily17V1, pawnio_bin,
                &load_warning);
            if (last_load_status == PawnIoStatus::ok) {
                break;
            }

            CloseHandle(pawnio_handle);
            pawnio_handle = INVALID_HANDLE_VALUE;
            last_status = MapPawnIoStatus(last_load_status);
            failed_during_open = false;
            if (attempt + 1u >= kPawnIoOpenAttempts ||
                !IsRetryablePawnIoLoadStatus(last_load_status)) {
                break;
            }
            SleepBeforePawnIoRetry(attempt);
        }

        if (pawnio_handle == INVALID_HANDLE_VALUE) {
            if (warning_text != nullptr) {
                if (failed_during_open) {
                    *warning_text =
                        std::string("PawnIO device is not available: path=") +
                        kPawnIoDevicePath + " win32_error=" +
                        std::to_string(last_open_error) + " status=" +
                        StatusString(last_status) + " attempts=" +
                        std::to_string(attempts);
                } else {
                    *warning_text = load_warning.empty()
                        ? std::string("Failed to load AMDFamily17.bin into PawnIO: path=") +
                              pawnio_bin.string() + " status=" +
                              PawnIoStatusString(last_load_status) +
                              " attempts=" + std::to_string(attempts)
                        : load_warning + " path=" + pawnio_bin.string() +
                              " attempts=" + std::to_string(attempts);
                }
            }
            return last_status;
        }

        handle = pawnio_handle;
        mutex_handle = OpenOrCreatePciMutex();
        cpu_name = std::move(detected_cpu_name);
        transport_path = pawnio_bin.string();
        supports_ccd = amd::SelectCcdLayout(cpu_model, &ccd_base);
        ccd_count_hint = 0u;
        initialized = true;
        if (warning_text != nullptr) {
            // Preserve warn_only hash-mismatch text so AmdReader::init_warning
            // surfaces it even when the load itself succeeded.
            *warning_text = load_warning;
        }
        return Status::ok;
    }

    // Performs one SMN read. The caller must already hold the
    // Global\Access_PCI mutex: AmdReader::Sample acquires it once for the
    // whole Tctl+CCD sequence (see Sample) instead of once per read, so a
    // steady-state control tick costs one mutex acquire/release rather than
    // up to nine and yields an interleave-free SMN snapshot. Do not add a
    // per-call lock here without revisiting Sample's single-lock scope.
    Status ReadSmnLocked(std::uint32_t smn_address,
                         std::uint32_t* out_value) const {
        if (!initialized || handle == nullptr || out_value == nullptr) {
            return Status::invalid_arg;
        }

        const std::int64_t input = static_cast<std::int64_t>(smn_address);
        std::int64_t output = 0;
        const Status exec_status = ExecutePawnIo(
            handle, "ioctl_read_smn", &input, 1u, &output, 1u);

        if (exec_status != Status::ok) {
            return exec_status;
        }
        *out_value = static_cast<std::uint32_t>(output);
        return Status::ok;
    }
};

AmdReader::AmdReader() : impl_(std::make_unique<Impl>()) {
    const std::string sim_mode = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE", "");
    if (sim_mode == "disabled") {
        impl_->init_warning = "direct AMD reader disabled by environment";
        return;
    }
    if (sim_mode == "enabled") {
        impl_->sim_mode = true;
        impl_->initialized = true;
        impl_->cpu_name = "Simulated AMD CPU";
        impl_->transport_path = "simulated-direct-amd";
        impl_->sim_tctl_sequence = ParseDoubleSequence(
            GetEnvOrDefault("SVG_MB_CONTROL_SIM_AMD_TCTL_SEQUENCE_C", ""));
        return;
    }

    std::string warning;
    const Status status = impl_->OpenReal(&warning);
    if (status != Status::ok) {
        impl_->init_warning = warning.empty()
            ? std::string("amd reader init failed: ") + StatusString(status)
            : warning;
        return;
    }
    // OpenReal may return ok with a non-empty warning (e.g. warn_only hash
    // mismatch from the PawnIO loader); keep that text so init_warning()
    // surfaces the diagnostic.
    impl_->init_warning = warning;
}

AmdReader::~AmdReader() = default;

bool AmdReader::available() const {
    return impl_ != nullptr && impl_->initialized;
}

std::string AmdReader::init_warning() const {
    return impl_ ? impl_->init_warning : std::string();
}

const AmdSnapshot& AmdReader::Sample() {
    // Reset every field before any early-return branch so no value carries
    // over from the previous Sample() call into the reused buffer. clear()
    // retains the samples vector and string capacities across ticks.
    AmdSnapshot& snapshot = sample_buffer_;
    snapshot.available = false;
    snapshot.samples.clear();
    snapshot.cpu_name.clear();
    snapshot.transport_path.clear();
    snapshot.last_warning.clear();
    if (impl_ == nullptr) {
        snapshot.last_warning = "not initialized";
        return snapshot;
    }

    snapshot.cpu_name = impl_->cpu_name;
    snapshot.transport_path = impl_->transport_path;

    if (!available()) {
        snapshot.last_warning = init_warning();
        return snapshot;
    }

    if (impl_->sim_mode) {
        double tctl = 65.0;
        if (!impl_->sim_tctl_sequence.empty()) {
            const std::size_t index =
                (std::min)(impl_->sim_tctl_index,
                           impl_->sim_tctl_sequence.size() - 1u);
            tctl = impl_->sim_tctl_sequence[index];
            if (impl_->sim_tctl_index + 1u < impl_->sim_tctl_sequence.size()) {
                ++impl_->sim_tctl_index;
            }
        } else {
            tctl = TryParseDoubleEnv("SVG_MB_CONTROL_SIM_AMD_TCTL_C")
                       .value_or(TryParseDoubleEnv(
                                     "SVG_MB_CONTROL_SIM_CPU_TCTL_C")
                                     .value_or(65.0));
        }
        snapshot.samples.push_back(AmdTemperatureSample{
            .label = "Tctl/Tdie",
            .temperature_c = tctl,
            .sensor_index = 0u,
            .raw_value = 0u,
        });
        snapshot.available = true;
        return snapshot;
    }

    // Acquire the system-wide PCI mutex once for the whole Tctl + CCD
    // sequence rather than per SMN read. This drops a steady-state control
    // tick from up to nine mutex acquire/release pairs to one, cuts the
    // chance of hitting the per-read timeout mid-sample, and yields an
    // interleave-free snapshot. pci_lock releases via RAII on every return
    // path below. The warning string mirrors the previous per-read
    // Tctl-failure message so the observable failure mode is unchanged.
    PciMutexLock pci_lock(impl_->mutex_handle);
    if (impl_->mutex_handle != nullptr && !pci_lock.acquired()) {
        snapshot.last_warning =
            std::string("read_tctl_tdie failed: ") +
            StatusString(Status::error);
        return snapshot;
    }

    std::uint32_t raw = 0u;
    Status status = impl_->ReadSmnLocked(kTctlTdieAddress, &raw);
    if (status != Status::ok) {
        snapshot.last_warning =
            std::string("read_tctl_tdie failed: ") + StatusString(status);
        return snapshot;
    }

    snapshot.samples.push_back(AmdTemperatureSample{
        .label = "Tctl/Tdie",
        .temperature_c = amd::DecodeTctl(raw),
        .sensor_index = 0u,
        .raw_value = raw,
    });

    if (impl_->supports_ccd) {
        std::uint32_t valid_ccds = 0u;
        for (std::uint32_t index = 0u; index < kMaxCcds; ++index) {
            std::uint32_t ccd_raw = 0u;
            status =
                impl_->ReadSmnLocked(impl_->ccd_base + (index * 4u), &ccd_raw);
            if (status != Status::ok) {
                break;
            }

            bool valid = false;
            const double ccd_temp = amd::DecodeCcdTemp(ccd_raw, &valid);
            if (!valid) {
                continue;
            }

            snapshot.samples.push_back(AmdTemperatureSample{
                .label = kCcdSensorLabels[index],
                .temperature_c = ccd_temp,
                .sensor_index = index + 1u,
                .raw_value = ccd_raw,
            });
            ++valid_ccds;
        }
        impl_->ccd_count_hint = valid_ccds;
    }

    snapshot.available = !snapshot.samples.empty();
    if (!snapshot.available) {
        snapshot.last_warning = "no AMD temperature samples were produced";
    }
    return snapshot;
}

}  // namespace svg_mb_control
