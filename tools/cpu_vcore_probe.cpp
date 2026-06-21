// Throwaway one-shot read-only probe: discover whether live CPU core/SoC voltage
// (SVI telemetry) is reachable on this Zen5 / Family-1Ah part (Ryzen 9 9950X3D)
// through the SHIPPED PawnIO AMDFamily17 bin's `ioctl_read_smn` path -- the same
// arbitrary-32-bit-SMN-address transport AmdReader already uses for Tctl/Tdie
// (0x00059800) and per-CCD Tdie (0x00059B08 + idx*4). The MSR allow-list
// (energy + APERF/MPERF) does NOT apply to the SMN path, so reading a voltage
// telemetry register needs no new kernel module and no allow-list change -- only
// the correct Family-1Ah SVI3 plane ADDRESS + DECODE, which is unknown and is
// exactly what this probe is for.
//
// NOTHING here is trusted output. The SVI plane address (0x0005A00C / 0x0005A010
// / 0x0005A014) and the decode (1.55 V - 6.25 mV * VID, VID = bits[23:16]) are
// SVI2-era constants from zenpower (Family 17h, model <= 0x71). Zen5 uses SVI3 and
// no open driver publishes a confirmed Family-1Ah plane address/decode, so the
// probe SWEEPS the 0x0005A000..0x0005A024 window, prints raw + candidate decodes
// at idle and under a busy spin, and leaves the verdict to a human comparison vs
// HWiNFO64. Promote an address into src/hardware ONLY after it passes the
// three-part discriminator printed at the end (match HWiNFO at idle AND load;
// track load; read measurably BELOW the pre-(-25 CO) regime at matched load).
//
// Read-only: issues only `ioctl_read_smn` (and `ioctl_read_msr` for nothing here);
// never writes; does not link svg_mb_control_core; reuses only the bin-load API
// (pawnio_binary.h) and the pure decode header (amd_decode.h) and REPLICATES the
// small read-only execute IOCTL -- the same call shape as AmdReader::ExecutePawnIo.
//
// OFF by default (SVG_MB_CONTROL_BUILD_VCORE_PROBE); never built by Test-LocalCI /
// Build-Release / CTest and never shipped in release\. Build explicitly with
//   cmake -DSVG_MB_CONTROL_BUILD_VCORE_PROBE=ON ... && cmake --build ... \
//         --target cpu_vcore_probe
// Run elevated, controller monitor-only. For the clearest load-tracking signal,
// run an all-core load (e.g. cpu-synth-load.exe) in another window during the run.

#include "windows_lean.h"

#include "amd_decode.h"
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

// Known-good SMN addresses (already read live by AmdReader -> transport sanity).
constexpr std::uint32_t kSmnTctlTdie = 0x00059800u;  // Tctl/Tdie composite
constexpr std::uint32_t kSmnCcdBaseZen4 = 0x00059B08u;  // per-CCD Tdie base (0x44)

// SVI telemetry sweep window. UNVERIFIED for SVI3 / Family 1Ah -- the whole point.
// SVI2-era references (zenpower): base 0x0005A000; TEL_PLANE0/1 near 0x5A00C..14.
constexpr std::uint32_t kSmnSviSweepLo = 0x0005A000u;
constexpr std::uint32_t kSmnSviSweepHi = 0x0005A024u;  // inclusive, step 4
constexpr std::uint32_t kSmnSviStep = 0x4u;

// Issue one read-only SMN read. Returns true on success (driver allowed + read
// succeeded); false when the driver denies the address or the read faults.
bool ReadSmn(HANDLE handle, std::uint32_t smn_address, std::uint64_t* out_value) {
    std::array<std::uint8_t, kPawnIoFnNameLength + sizeof(std::int64_t)> in_buffer{};
    std::array<std::uint8_t, sizeof(std::int64_t)> out_buffer{};
    std::snprintf(reinterpret_cast<char*>(in_buffer.data()), kPawnIoFnNameLength,
                  "%s", "ioctl_read_smn");
    const std::int64_t arg = static_cast<std::int64_t>(smn_address);
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

// Candidate SVI voltage decode (UNVERIFIED, SVI2 linear map): the voltage VID is
// bits [23:16]; volts = 1.55 - 0.00625 * VID. SVI3 may have moved the field or
// changed the scale, so this is one hypothesis to test against HWiNFO64, not a
// fact. Returns volts for the given 8-bit field interpreted as a VID.
double DecodeSviVoltageCandidate(std::uint32_t raw) {
    const std::uint32_t vid = (raw >> 16) & 0xFFu;
    return 1.55 - 0.00625 * static_cast<double>(vid);
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

// Sweep the SVI window once and print raw + the candidate voltage decode for the
// VID field, so a human can spot which address(es) carry a plausible ~0.9-1.4 V
// reading. `phase` labels the schedule state (idle / busy).
void SweepSvi(HANDLE handle, const char* phase) {
    std::printf("-- SVI sweep [%s] (raw32 + candidate VID-field voltage) --\n",
                phase);
    for (std::uint32_t addr = kSmnSviSweepLo; addr <= kSmnSviSweepHi;
         addr += kSmnSviStep) {
        std::uint64_t raw = 0;
        if (!ReadSmn(handle, addr, &raw)) {
            std::printf("  0x%08lX : DENIED/fault\n",
                        static_cast<unsigned long>(addr));
            continue;
        }
        const std::uint32_t raw32 = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
        std::printf("  0x%08lX : raw=0x%08lX  vid[23:16]=%3u  V~=%.4f\n",
                    static_cast<unsigned long>(addr),
                    static_cast<unsigned long>(raw32), (raw32 >> 16) & 0xFFu,
                    DecodeSviVoltageCandidate(raw32));
    }
}

}  // namespace

int main() {
    std::printf("== CPU Vcore SVI-telemetry probe (read-only, SMN sweep) ==\n");
    std::printf("   Zen5 / Family 1Ah (9950X3D). Addresses/decode UNVERIFIED.\n");

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

    const std::filesystem::path bin = svg_mb_control::ResolvePawnIoBinaryPath(
        svg_mb_control::kPawnIoSpecAmdFamily17V1);
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

    // Transport sanity: the SMN path must already work for the known temperature
    // registers, isolating any SVI failure to the address/decode (not the bin).
    std::uint64_t raw = 0;
    if (ReadSmn(handle, kSmnTctlTdie, &raw)) {
        std::printf("[sanity] Tctl/Tdie 0x%08lX -> %.3f C (transport OK)\n",
                    static_cast<unsigned long>(kSmnTctlTdie),
                    svg_mb_control::amd::DecodeTctl(
                        static_cast<std::uint32_t>(raw & 0xFFFFFFFFu)));
    } else {
        std::printf("[sanity] Tctl read DENIED -- SMN transport unavailable; the"
                    " SVI sweep below will not be meaningful.\n");
    }
    if (ReadSmn(handle, kSmnCcdBaseZen4, &raw)) {
        bool valid = false;
        const double ccd0 = svg_mb_control::amd::DecodeCcdTemp(
            static_cast<std::uint32_t>(raw & 0xFFFFFFFFu), &valid);
        std::printf("[sanity] CCD0 Tdie  0x%08lX -> %.3f C valid=%d\n",
                    static_cast<unsigned long>(kSmnCcdBaseZen4), ccd0,
                    valid ? 1 : 0);
    }

    SweepSvi(handle, "idle");
    std::printf("   (holding all-cores-spin for the load comparison)...\n");
    BusySpinMs(800u);
    SweepSvi(handle, "busy(single-thread spin)");

    std::printf(
        "== verdict is MANUAL. A candidate address carries Vcore only if its"
        " decoded voltage (a) matches HWiNFO64 'CPU Core Voltage (SVI2 TFN)' at"
        " idle AND under an all-core load, (b) RISES from idle->busy, and (c)"
        " reads measurably below the pre(-25 CO) regime at matched load. A second"
        " near-constant ~1.0-1.2 V address that does NOT track core load is the"
        " SoC (VSoC) plane. Do NOT promote any address that fails all three. ==\n");
    CloseHandle(handle);
    return 0;
}
