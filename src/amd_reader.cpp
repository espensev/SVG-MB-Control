#include "amd_reader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

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

constexpr std::uint32_t kTempOffsetFlag = 0x80000u;
constexpr std::uint32_t kTctlTdieAddress = 0x00059800u;
constexpr std::uint32_t kCcdTempZen2Base = 0x00059954u;
constexpr std::uint32_t kCcdTempZen4Base = 0x00059B08u;
constexpr std::uint32_t kMaxCcds = 8u;
constexpr const char kPawnIoDevicePath[] = "\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr std::uint32_t kPawnIoLoadBinary = (41394u << 16) | (0x821u << 2);
constexpr std::uint32_t kPawnIoExecuteFn = (41394u << 16) | (0x841u << 2);
constexpr std::size_t kPawnIoFnNameLength = 32u;
constexpr DWORD kPciMutexTimeoutMs = 100u;
constexpr DWORD kPciMutexAccess = SYNCHRONIZE | MUTEX_MODIFY_STATE;

std::string GetEnvOrDefault(const char* name, std::string_view fallback) {
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr ||
        value[0] == '\0') {
        if (value != nullptr) {
            std::free(value);
        }
        return std::string(fallback);
    }
    std::string result(value);
    std::free(value);
    return result;
}

std::filesystem::path CurrentExecutableDirectory() {
    std::array<char, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(
               std::string(buffer.data(), buffer.data() + length))
        .parent_path();
}

void AddUniquePath(std::vector<std::filesystem::path>& paths,
                   const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return;
    }
    const std::filesystem::path normalized = candidate.lexically_normal();
    if (std::find(paths.begin(), paths.end(), normalized) == paths.end()) {
        paths.push_back(normalized);
    }
}

void AddRootAndParents(std::vector<std::filesystem::path>& paths,
                       std::filesystem::path root,
                       std::size_t max_depth) {
    for (std::size_t depth = 0u; depth < max_depth && !root.empty(); ++depth) {
        AddUniquePath(paths, root);
        const std::filesystem::path parent = root.parent_path();
        if (parent == root) {
            break;
        }
        root = parent;
    }
}

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

HANDLE OpenOrCreatePciMutex() {
    HANDLE handle = OpenMutexA(kPciMutexAccess, FALSE, "Global\\Access_PCI");
    if (handle != nullptr) {
        return handle;
    }
    return CreateMutexA(nullptr, FALSE, "Global\\Access_PCI");
}

std::filesystem::path ResolvePawnIoBinaryPath() {
    const std::array<const char*, 2> env_names = {
        "SVG_MB_CONTROL_PAWNIO_BIN",
        "SVG_MB_PAWNIO_BIN",
    };
    for (const char* env_name : env_names) {
        const std::string env_value = GetEnvOrDefault(env_name, "");
        if (env_value.empty()) {
            continue;
        }
        const std::filesystem::path candidate(env_value);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }

    std::vector<std::filesystem::path> search_roots;
    AddRootAndParents(search_roots, CurrentExecutableDirectory(), 6u);

    std::error_code cwd_ec;
    AddRootAndParents(search_roots, std::filesystem::current_path(cwd_ec), 6u);

    for (const auto& root : search_roots) {
        for (const auto& candidate : std::array<std::filesystem::path, 3>{
                 root / "resources" / "pawnio" / "AMDFamily17.bin",
                 root / "release" / "resources" / "pawnio" / "AMDFamily17.bin",
                 root / "dist" / "resources" / "pawnio" / "AMDFamily17.bin",
             }) {
            std::error_code exists_ec;
            if (std::filesystem::exists(candidate, exists_ec) &&
                !std::filesystem::is_directory(candidate, exists_ec)) {
                return std::filesystem::absolute(candidate).lexically_normal();
            }
        }
    }

    return {};
}

Status LoadPawnIoBinary(HANDLE handle, const std::filesystem::path& bin_path) {
    if (handle == nullptr || bin_path.empty()) {
        return Status::invalid_arg;
    }

    HANDLE file_handle = CreateFileA(
        bin_path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return StatusFromWin32Error(GetLastError());
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_handle, &file_size) || file_size.QuadPart <= 0 ||
        file_size.QuadPart > 1024 * 1024) {
        CloseHandle(file_handle);
        return Status::error;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(file_size.QuadPart));
    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(file_handle, buffer.data(),
                                  static_cast<DWORD>(buffer.size()),
                                  &bytes_read, nullptr);
    CloseHandle(file_handle);
    if (!read_ok || bytes_read != buffer.size()) {
        return Status::error;
    }

    DWORD bytes_returned = 0;
    const BOOL ioctl_ok = DeviceIoControl(handle, kPawnIoLoadBinary,
                                          buffer.data(), bytes_read, nullptr, 0,
                                          &bytes_returned, nullptr);
    return ioctl_ok ? Status::ok : Status::error;
}

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

bool SelectCcdLayout(std::uint32_t cpu_model, std::uint32_t* out_base) {
    switch (cpu_model) {
        case 0x31u:
        case 0x71u:
        case 0x21u:
            if (out_base != nullptr) {
                *out_base = kCcdTempZen2Base;
            }
            return true;
        case 0x61u:
        case 0x44u:
            if (out_base != nullptr) {
                *out_base = kCcdTempZen4Base;
            }
            return true;
        default:
            if (out_base != nullptr) {
                *out_base = 0u;
            }
            return false;
    }
}

double DecodeTctl(std::uint32_t raw) {
    double temp = static_cast<double>(((raw >> 21) * 125u) * 0.001);
    if ((raw & kTempOffsetFlag) != 0u) {
        temp -= 49.0;
    }
    return temp;
}

double DecodeCcdTemp(std::uint32_t raw, bool* out_valid) {
    const std::uint32_t raw_12bit = raw & 0xFFFu;
    const double temp =
        ((static_cast<double>(raw_12bit * 125u) - 305000.0) * 0.001);
    if (out_valid != nullptr) {
        *out_valid = (raw_12bit > 0u && temp < 125.0);
    }
    return temp;
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

        const std::filesystem::path pawnio_bin = ResolvePawnIoBinaryPath();
        if (pawnio_bin.empty()) {
            if (warning_text != nullptr) {
                *warning_text =
                    "AMDFamily17.bin was not found. Set "
                    "SVG_MB_CONTROL_PAWNIO_BIN or SVG_MB_PAWNIO_BIN, or keep "
                    "a packaged resources\\pawnio copy available.";
            }
            return Status::not_supported;
        }

        HANDLE pawnio_handle = CreateFileA(
            kPawnIoDevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
            nullptr);
        if (pawnio_handle == INVALID_HANDLE_VALUE) {
            if (warning_text != nullptr) {
                *warning_text = "PawnIO device is not available.";
            }
            return StatusFromWin32Error(GetLastError());
        }

        const Status load_status = LoadPawnIoBinary(pawnio_handle, pawnio_bin);
        if (load_status != Status::ok) {
            CloseHandle(pawnio_handle);
            if (warning_text != nullptr) {
                *warning_text = "Failed to load AMDFamily17.bin into PawnIO.";
            }
            return load_status;
        }

        handle = pawnio_handle;
        mutex_handle = OpenOrCreatePciMutex();
        cpu_name = std::move(detected_cpu_name);
        transport_path = pawnio_bin.string();
        supports_ccd = SelectCcdLayout(cpu_model, &ccd_base);
        ccd_count_hint = 0u;
        initialized = true;
        if (warning_text != nullptr) {
            warning_text->clear();
        }
        return Status::ok;
    }

    Status ReadSmn(std::uint32_t smn_address, std::uint32_t* out_value) const {
        if (!initialized || handle == nullptr || out_value == nullptr) {
            return Status::invalid_arg;
        }

        DWORD wait_result = WAIT_OBJECT_0;
        if (mutex_handle != nullptr) {
            wait_result =
                WaitForSingleObject(mutex_handle, kPciMutexTimeoutMs);
            if (wait_result != WAIT_OBJECT_0 &&
                wait_result != WAIT_ABANDONED) {
                return Status::error;
            }
        }

        const std::int64_t input = static_cast<std::int64_t>(smn_address);
        std::int64_t output = 0;
        const Status exec_status = ExecutePawnIo(
            handle, "ioctl_read_smn", &input, 1u, &output, 1u);

        if (mutex_handle != nullptr &&
            (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED)) {
            ReleaseMutex(mutex_handle);
        }

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
    impl_->init_warning.clear();
}

AmdReader::~AmdReader() = default;

bool AmdReader::available() const {
    return impl_ != nullptr && impl_->initialized;
}

std::string AmdReader::init_warning() const {
    return impl_ ? impl_->init_warning : std::string();
}

AmdSnapshot AmdReader::Sample() {
    AmdSnapshot snapshot;
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

    std::uint32_t raw = 0u;
    Status status = impl_->ReadSmn(kTctlTdieAddress, &raw);
    if (status != Status::ok) {
        snapshot.last_warning =
            std::string("read_tctl_tdie failed: ") + StatusString(status);
        return snapshot;
    }

    snapshot.samples.push_back(AmdTemperatureSample{
        .label = "Tctl/Tdie",
        .temperature_c = DecodeTctl(raw),
        .sensor_index = 0u,
        .raw_value = raw,
    });

    if (impl_->supports_ccd) {
        std::uint32_t valid_ccds = 0u;
        for (std::uint32_t index = 0u; index < kMaxCcds; ++index) {
            std::uint32_t ccd_raw = 0u;
            status = impl_->ReadSmn(impl_->ccd_base + (index * 4u), &ccd_raw);
            if (status != Status::ok) {
                break;
            }

            bool valid = false;
            const double ccd_temp = DecodeCcdTemp(ccd_raw, &valid);
            if (!valid) {
                continue;
            }

            snapshot.samples.push_back(AmdTemperatureSample{
                .label = "CCD" + std::to_string(index + 1u) + " (Tdie)",
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
