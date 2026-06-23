#include "machine_identity.h"

#include "windows_lean.h"

#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace svg_mb_control {

namespace {

std::string Trim(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n\f\v";
    const auto begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) {
        return std::string();
    }
    const auto end = text.find_last_not_of(kWhitespace);
    return std::string(text.substr(begin, end - begin + 1));
}

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

}  // namespace

std::string ResolveMachineIdFrom(std::string_view raw_host_name,
                                 std::string_view override_file_contents) {
    // A non-empty override (trimmed) is the operator's explicit machine id and
    // wins; it is used verbatim so an operator can name any catalog profile.
    const std::string override_id = Trim(override_file_contents);
    if (!override_id.empty()) {
        return override_id;
    }
    // Otherwise use the host name, trimmed and lower-cased so it matches a
    // lower-case catalog profile name regardless of host-name casing.
    return ToLowerAscii(Trim(raw_host_name));
}

std::string ResolveMachineId(
    const std::filesystem::path& machine_id_override_file) {
    std::string override_contents;
    {
        std::ifstream in(machine_id_override_file, std::ios::binary);
        if (in) {
            std::ostringstream buffer;
            buffer << in.rdbuf();
            override_contents = buffer.str();
        }
    }

    std::string host_name;
    std::array<wchar_t, 256> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (GetComputerNameW(buffer.data(), &size)) {
        for (DWORD i = 0; i < size; ++i) {
            const wchar_t wide = buffer[i];
            // Computer names are ASCII; the override file is the escape hatch
            // for any host name that does not narrow cleanly.
            if (wide > 0 && wide < 128) {
                host_name.push_back(static_cast<char>(wide));
            }
        }
    }

    return ResolveMachineIdFrom(host_name, override_contents);
}

}  // namespace svg_mb_control
