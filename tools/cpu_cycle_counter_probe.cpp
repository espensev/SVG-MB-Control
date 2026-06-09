// Throwaway one-shot read-only probe: confirm the SHIPPED PawnIO AMDFamily17 bin
// services the AMD read-only APERF/MPERF aliases (MPERF_RO 0xC00000E7 / APERF_RO
// 0xC00000E8). The 2026-06-07 FEAT-0006 gate probe concluded "APERF/MPERF
// unreachable" but read the architectural indices 0xE7/0xE8 (and TSC_AUX
// 0xC0000103), which AMDFamily17.p does NOT allow-list -- it allow-lists the AMD
// read-only aliases 0xC00000E7/0xC00000E8 instead. This probe reads the correct
// indices to close the work-numerator / effective-frequency question (FEAT-0006
// REQ-CPUEFF-01/03, docs/cpu-cycle-counter-source-decision-2026-06-07.md).
//
// Read-only: issues only `ioctl_read_msr`; never writes; does not link
// svg_mb_control_core and does not touch src/hardware. It reuses only the
// bin-load API (pawnio_binary.h) and REPLICATES the small read-only execute
// IOCTL -- the same call shape as AmdReader's ExecutePawnIo.
//
// OFF by default (SVG_MB_CONTROL_BUILD_CYCLE_PROBE); never built by Test-LocalCI
// / Build-Release / CTest and never shipped in release\. Build explicitly with
//   cmake -DSVG_MB_CONTROL_BUILD_CYCLE_PROBE=ON ... && cmake --build ... \
//         --target cpu_cycle_counter_probe
// Run elevated, controller monitor-only.

#include "windows_lean.h"

#include "pawnio_binary.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Replicated from src/hardware/amd_reader.cpp (kept local so the probe does not
// link svg_mb_control_core). PawnIO execute IOCTL + 32-byte function-name prefix.
constexpr char kPawnIoDevicePath[] = "\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr DWORD kPawnIoExecuteFn = (41394u << 16) | (0x841u << 2);
constexpr std::size_t kPawnIoFnNameLength = 32u;

// AMD read-only MPERF/APERF aliases (AMD OSRR 56255): MperfReadOnly counts at P0
// frequency while in C0; AperfReadOnly counts at the actual core clock while in
// C0; effective frequency = (APERF/MPERF) * P0. These are what AMDFamily17.p
// allow-lists, NOT the architectural 0xE7/0xE8.
constexpr std::uint32_t kMsrMperfRo = 0xC00000E7u;
constexpr std::uint32_t kMsrAperfRo = 0xC00000E8u;
constexpr std::uint32_t kMsrIa32Mperf = 0x000000E7u;  // architectural; expect DENIED
constexpr std::uint32_t kMsrIa32Aperf = 0x000000E8u;  // architectural; expect DENIED
constexpr std::uint32_t kMsrPkgEnergy = 0xC001029Bu;  // known-good allow-listed read

// Issue ioctl_read_msr for one index. Returns true on success (driver allowed +
// read succeeded); false when the driver denies the index or the read faults.
bool ReadMsr(HANDLE handle, std::uint32_t msr, std::uint64_t* out_value) {
    std::array<std::uint8_t, kPawnIoFnNameLength + sizeof(std::int64_t)> in_buffer{};
    std::array<std::uint8_t, sizeof(std::int64_t)> out_buffer{};
    std::snprintf(reinterpret_cast<char*>(in_buffer.data()), kPawnIoFnNameLength,
                  "%s", "ioctl_read_msr");
    const std::int64_t arg = static_cast<std::int64_t>(msr);
    std::memcpy(in_buffer.data() + kPawnIoFnNameLength, &arg, sizeof(arg));

    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
        handle, kPawnIoExecuteFn, in_buffer.data(),
        static_cast<DWORD>(in_buffer.size()), out_buffer.data(),
        static_cast<DWORD>(out_buffer.size()), &bytes_returned, nullptr);
    if (!ok || bytes_returned < sizeof(std::int64_t)) {
        return false;
    }
    std::int64_t value = 0;
    std::memcpy(&value, out_buffer.data(), sizeof(value));
    *out_value = static_cast<std::uint64_t>(value);
    return true;
}

// Pin the calling thread to one core and confirm we are scheduled there, so the
// per-core APERF/MPERF reads are coherent (the Q1 affinity question).
bool PinToCore(DWORD core) {
    if (SetThreadAffinityMask(GetCurrentThread(),
                              static_cast<DWORD_PTR>(1) << core) == 0) {
        return false;
    }
    for (int i = 0; i < 1000; ++i) {
        if (GetCurrentProcessorNumber() == core) {
            return true;
        }
        SwitchToThread();
    }
    return GetCurrentProcessorNumber() == core;
}

void BusySpinMs(unsigned ms) {
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    volatile std::uint64_t acc = 0;
    do {
        for (int i = 0; i < 200000; ++i) {
            acc += static_cast<std::uint64_t>(i);
        }
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) * 1000 < freq.QuadPart * ms);
}

// Read MPERF_RO/APERF_RO at t0, hold the core busy or idle for `ms`, read again,
// and report the deltas + the APERF/MPERF ratio. Effective freq = ratio * P0.
void SampleRatio(HANDLE handle, const char* label, bool busy, unsigned ms) {
    std::uint64_t m0 = 0;
    std::uint64_t a0 = 0;
    std::uint64_t m1 = 0;
    std::uint64_t a1 = 0;
    if (!ReadMsr(handle, kMsrMperfRo, &m0) || !ReadMsr(handle, kMsrAperfRo, &a0)) {
        std::printf("[%s]  read failed\n", label);
        return;
    }
    if (busy) {
        BusySpinMs(ms);
    } else {
        Sleep(ms);
    }
    if (!ReadMsr(handle, kMsrMperfRo, &m1) || !ReadMsr(handle, kMsrAperfRo, &a1)) {
        std::printf("[%s]  read failed\n", label);
        return;
    }
    const std::uint64_t d_mperf = m1 - m0;  // 64-bit, monotonic over the window
    const std::uint64_t d_aperf = a1 - a0;
    const double ratio =
        d_mperf != 0u ? static_cast<double>(d_aperf) / static_cast<double>(d_mperf)
                      : 0.0;
    std::printf("[%s]  dMPERF=%llu dAPERF=%llu  APERF/MPERF=%.3f\n", label,
                static_cast<unsigned long long>(d_mperf),
                static_cast<unsigned long long>(d_aperf), ratio);
}

}  // namespace

int main() {
    std::printf("== FEAT-0006 cycle-counter probe (read-only, corrected indices) ==\n");

    HANDLE handle =
        CreateFileA(kPawnIoDevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf(
            "FAIL: cannot open PawnIO device (run elevated; PawnIO installed?)."
            " err=%lu\n",
            GetLastError());
        return 2;
    }

    const std::filesystem::path bin =
        svg_mb_control::ResolvePawnIoBinaryPath(svg_mb_control::kPawnIoSpecAmdFamily17V1);
    std::string warning;
    bool hash_mismatch = false;
    const svg_mb_control::PawnIoStatus load = svg_mb_control::LoadPawnIoBinary(
        handle, svg_mb_control::kPawnIoSpecAmdFamily17V1, bin, &warning,
        &hash_mismatch);
    std::printf("bin:  %s\nload: %s%s\n", bin.string().c_str(),
                svg_mb_control::PawnIoStatusString(load),
                hash_mismatch ? "  (HASH MISMATCH)" : "");
    if (load != svg_mb_control::PawnIoStatus::ok) {
        CloseHandle(handle);
        return 2;
    }

    std::uint64_t value = 0;
    std::printf("[sanity] pkg_energy   0xC001029B: %s\n",
                ReadMsr(handle, kMsrPkgEnergy, &value) ? "ok" : "DENIED");
    std::printf("[reach]  MPERF_RO     0xC00000E7: %s\n",
                ReadMsr(handle, kMsrMperfRo, &value) ? "ok" : "DENIED");
    std::printf("[reach]  APERF_RO     0xC00000E8: %s\n",
                ReadMsr(handle, kMsrAperfRo, &value) ? "ok" : "DENIED");
    std::printf("[reach]  IA32_MPERF   0x000000E7: %s (expect DENIED)\n",
                ReadMsr(handle, kMsrIa32Mperf, &value) ? "ok" : "DENIED");
    std::printf("[reach]  IA32_APERF   0x000000E8: %s (expect DENIED)\n",
                ReadMsr(handle, kMsrIa32Aperf, &value) ? "ok" : "DENIED");

    if (PinToCore(0)) {
        std::printf("[affinity] pinned to core 0 (confirmed)\n");
    } else {
        std::printf("[affinity] WARN: could not confirm pin to core 0 (proc=%lu)\n",
                    GetCurrentProcessorNumber());
    }
    SampleRatio(handle, "idle", false, 500u);
    SampleRatio(handle, "busy", true, 500u);

    std::printf(
        "== verdict: RO aliases reachable -> work numerator (dAPERF) + effective"
        " frequency (dAPERF/dMPERF x P0) obtainable with the SHIPPED bin;"
        " confirm the busy ratio is plausible and exceeds idle ==\n");
    CloseHandle(handle);
    return 0;
}
