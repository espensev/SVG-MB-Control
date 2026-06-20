#pragma once

// FEAT-0023 machine identity. Resolves a machine id used to auto-select a
// profile from the catalog when no explicit --config / --profile is given.

#include <filesystem>
#include <string>
#include <string_view>

namespace svg_mb_control {

// Resolves the machine id from the raw host name and the contents of an optional
// machine-id override file. Precedence: a non-empty (trimmed) override wins;
// otherwise the host name is used, trimmed and lower-cased so it matches a
// lower-case catalog profile name regardless of host-name casing. Returns an
// empty string when neither yields a non-empty id. Pure (no I/O) for testing.
std::string ResolveMachineIdFrom(std::string_view raw_host_name,
                                 std::string_view override_file_contents);

// Reads the host name (GetComputerNameW) and the optional override file, then
// returns ResolveMachineIdFrom(host_name, override_contents). A missing or
// unreadable override file is treated as empty override contents.
std::string ResolveMachineId(
    const std::filesystem::path& machine_id_override_file);

}  // namespace svg_mb_control
