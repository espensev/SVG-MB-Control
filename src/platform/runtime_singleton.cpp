#include "runtime_singleton.h"

#include "windows_lean.h"

#include <bcrypt.h>

#include <array>
#include <cstdio>
#include <cwctype>
#include <sstream>
#include <string>
#include <system_error>

namespace svg_mb_control {

namespace {

std::wstring CanonicalRuntimeHomeKey(const std::filesystem::path& runtime_home) {
    std::error_code ec;
    std::filesystem::path canonical =
        std::filesystem::weakly_canonical(runtime_home, ec);
    if (ec || canonical.empty()) {
        canonical = std::filesystem::absolute(runtime_home, ec);
        if (ec || canonical.empty()) {
            canonical = runtime_home;
        }
    }
    std::wstring key = canonical.lexically_normal().wstring();
    // Windows is case-insensitive for paths; lowercase the key so case-only
    // differences map to the same mutex.
    for (wchar_t& ch : key) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return key;
}

bool Sha256Hex(const std::wstring& source, std::string& out_hex) {
    out_hex.clear();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32];

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return false;
    }

    auto cleanup = [&]() {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    };

    status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        return false;
    }

    const ULONG byte_count =
        static_cast<ULONG>(source.size() * sizeof(wchar_t));
    status = BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(source.data())),
        byte_count, 0);
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        return false;
    }

    status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (!BCRYPT_SUCCESS(status)) {
        cleanup();
        return false;
    }

    cleanup();

    std::array<char, 65> hex{};
    for (int i = 0; i < 32; ++i) {
        std::snprintf(hex.data() + i * 2, 3, "%02x",
                      static_cast<unsigned int>(digest[i]));
    }
    out_hex.assign(hex.data(), 64);
    return true;
}

std::wstring SingletonMutexName(SingletonRole role,
                                const std::filesystem::path& runtime_home) {
    std::string digest;
    if (!Sha256Hex(CanonicalRuntimeHomeKey(runtime_home), digest)) {
        // BCrypt is present on every Windows version this codebase targets;
        // a failure here means the OS is in an unrecoverable state. Refuse
        // to construct a name rather than collide globally by reusing a
        // shared fallback (which would map every runtime_home on the system
        // to one mutex and silently re-introduce the duplicate-supervisor
        // race we just fixed).
        return {};
    }
    const std::wstring digest_w(digest.begin(), digest.end());
    const std::wstring role_part =
        role == SingletonRole::kSupervisor ? L"supervisor" : L"worker";
    return L"Local\\svg-mb-control." + role_part + L"." + digest_w;
}

}  // namespace

RuntimeSingletonLock::~RuntimeSingletonLock() {
    Release();
}

RuntimeSingletonLock::RuntimeSingletonLock(RuntimeSingletonLock&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

RuntimeSingletonLock& RuntimeSingletonLock::operator=(
    RuntimeSingletonLock&& other) noexcept {
    if (this != &other) {
        Release();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void RuntimeSingletonLock::Release() noexcept {
    if (handle_ != nullptr) {
        ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

void RuntimeSingletonLock::Adopt(void* handle) noexcept {
    Release();
    handle_ = handle;
}

SingletonAcquisition TryAcquireRuntimeSingleton(
    SingletonRole role,
    const std::filesystem::path& runtime_home) {
    SingletonAcquisition result;
    const std::wstring name = SingletonMutexName(role, runtime_home);
    if (name.empty()) {
        result.diagnostic = "BCryptHash failed; cannot derive singleton name";
        return result;
    }

    HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle == nullptr) {
        std::ostringstream os;
        os << "CreateMutexW failed (Windows error "
           << GetLastError() << ")";
        result.diagnostic = os.str();
        return result;
    }

    const DWORD wait = WaitForSingleObject(handle, 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        result.lock.Adopt(handle);
        result.acquired = true;
        return result;
    }
    if (wait == WAIT_TIMEOUT) {
        CloseHandle(handle);
        result.other_holder_present = true;
        std::ostringstream os;
        os << "another process holds the "
           << (role == SingletonRole::kSupervisor ? "supervisor" : "worker")
           << " singleton for runtime_home="
           << runtime_home.string();
        result.diagnostic = os.str();
        return result;
    }

    CloseHandle(handle);
    std::ostringstream os;
    os << "WaitForSingleObject returned 0x" << std::hex << wait
       << " (last_error=" << std::dec << GetLastError() << ")";
    result.diagnostic = os.str();
    return result;
}

bool ProbeRuntimeSingletonHeld(SingletonRole role,
                               const std::filesystem::path& runtime_home) {
    const std::wstring name = SingletonMutexName(role, runtime_home);
    if (name.empty()) {
        return false;
    }
    HANDLE handle = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE,
                               FALSE,
                               name.c_str());
    if (handle == nullptr) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(handle, 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        // We briefly acquired; release immediately so the supervisor child can
        // re-acquire authoritatively.
        ReleaseMutex(handle);
        CloseHandle(handle);
        return false;
    }
    CloseHandle(handle);
    return wait == WAIT_TIMEOUT;
}

}  // namespace svg_mb_control
