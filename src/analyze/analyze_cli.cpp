#include "analyze/analyze_cli.h"

#include "analyze/analyze_ingest.h"
#include "analyze/analyze_prune.h"
#include "analyze/analyze_report.h"
#include "control_config.h"
#include "read_loop.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace svg_mb_control::analyze {

namespace {

void PrintAnalyzeUsage() {
    std::cout
        << "Usage:\n"
        << "  svg-mb-control analyze ingest [--runtime-home <path>] "
           << "[--db <path>] [--force] [--quiet]\n"
        << "    Reads CSV archives, manifests, events.jsonl and "
           << "plant_model.json from the\n"
        << "    runtime home and ingests them into a sqlite database. "
           << "Default db path is\n"
        << "    <runtime-home>/svg_mb_control.db. Idempotent on "
           << "previously-seen artifacts\n"
        << "    unless --force is passed.\n";
    std::cout
        << "  svg-mb-control analyze ingest --csv <path> [--events <path>] "
           << "[--db <path>] [--force] [--quiet]\n"
        << "    Ingests a single control-loop CSV (and optional events JSONL) "
           << "with no runtime\n"
        << "    manifest, synthesizing one run row keyed on the CSV path.\n";
    std::cout
        << "  svg-mb-control analyze prune [--runtime-home <path>] "
           << "[--db <path>] [--retain-days <days>] [--dry-run|--apply] [--quiet]\n"
        << "    Finds old archive CSV/manifest bundles. Dry-run is the default; "
           << "--apply is\n"
        << "    required before files are deleted. Deletion is gated on the run "
           << "already\n"
        << "    being present in the sqlite ingest database.\n";
    std::cout
        << "  svg-mb-control analyze report [--runtime-home <path>] "
           << "[--db <path>] [--run <id>|--session <ts>] [--idle-seconds <s>] "
           << "[--load-threshold-c <c>] [--gpu-load-threshold-c <c>] "
           << "[--p0-mhz <mhz>] "
           << "[--json] [--out <path>] "
           << "[--manifest-out <path>] "
           << "[--decision-record-out <path|auto>|--no-decision-record] "
           << "[--profile <name>] [--hypothesis <text>] "
           << "[--decision <text>] [--notes <text>]\n"
        << "    Summarizes one ingested run from the sqlite database. Selects "
           << "the most\n"
        << "    recent run unless --run or --session is given. Reports idle/"
           << "load/cooldown\n"
        << "    p50/p90/max for CPU Tctl and GPU memory/envelope, per-channel "
           << "setpoint/\n"
        << "    duty/rpm and write reversals, response delay after the first "
           << "load-threshold\n"
        << "    crossing, and authority/write/restore failure counts. "
           << "Can write\n"
        << "    report, analysis-manifest, and Markdown decision-record "
           << "artifacts. Read-only.\n";
}

bool ResolveAnalyzeRuntimeHome(
    std::filesystem::path& runtime_home,
    std::filesystem::path config_path,
    bool config_path_explicit,
    std::optional<svg_mb_control::ControlConfig>& resolved_config) {
    if (config_path.empty()) {
        config_path = svg_mb_control::GetEnvironmentPath(
            L"SVG_MB_CONTROL_CONFIG");
    }
    if (config_path.empty()) {
        config_path = svg_mb_control::ResolveDefaultControlConfigPath();
    }

    if (!config_path.empty()) {
        const std::filesystem::path absolute_config_path =
            std::filesystem::absolute(config_path).lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(absolute_config_path, ec)) {
            try {
                resolved_config = svg_mb_control::LoadControlConfig(
                    absolute_config_path);
            } catch (const std::exception& error) {
                resolved_config.reset();
                // Analyzer continues without channel context, but an
                // operator-supplied path that fails to parse is worth
                // surfacing — silent fallback hides typos and schema drift.
                if (config_path_explicit) {
                    std::cerr << "Warning: failed to parse control config "
                              << absolute_config_path.string() << ": "
                              << error.what()
                              << " (continuing with default runtime home)\n";
                }
            }
        } else if (config_path_explicit) {
            std::cerr << "Error: control config not found: "
                      << absolute_config_path.string() << '\n';
            return false;
        }
    }

    if (runtime_home.empty()) {
        runtime_home = resolved_config.has_value()
            ? svg_mb_control::ResolveRuntimeHomePath(*resolved_config)
            : svg_mb_control::ResolveRuntimeHomePath(
                  svg_mb_control::ControlConfig{});
    }
    return true;
}

bool ParseUint32Option(const wchar_t* text, std::uint32_t& out) {
    try {
        const unsigned long parsed = std::stoul(std::wstring(text));
        if (parsed > UINT32_MAX) {
            return false;
        }
        out = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int RunAnalyzeCommand(int argc, wchar_t** argv) {
    if (argc < 3) {
        PrintAnalyzeUsage();
        return 1;
    }
    const std::wstring verb = argv[2];
    if (verb == L"--help" || verb == L"-h") {
        PrintAnalyzeUsage();
        return 0;
    }
    if (verb != L"ingest" && verb != L"prune" && verb != L"report") {
        std::cerr << "Error: unknown analyze subcommand. Try "
                  << "'svg-mb-control analyze --help'.\n";
        return 1;
    }

    svg_mb_control::analyze::IngestOptions options;
    svg_mb_control::analyze::PruneOptions prune_options;
    svg_mb_control::analyze::ReportOptions report_options;
    std::filesystem::path config_path;
    bool config_path_explicit = false;
    bool retain_days_explicit = false;

    auto only_for = [&](const wchar_t* required_verb, const char* verb_label,
                        const char* flag) -> bool {
        if (verb != required_verb) {
            std::cerr << "Error: " << flag << " is only valid for analyze "
                      << verb_label << ".\n";
            PrintAnalyzeUsage();
            return false;
        }
        return true;
    };
    auto report_only = [&](const char* flag) -> bool {
        return only_for(L"report", "report", flag);
    };

    auto require_value = [&](int& index) -> const wchar_t* {
        if (index + 1 >= argc) {
            throw std::runtime_error("Missing value for option.");
        }
        ++index;
        return argv[index];
    };

    for (int index = 3; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"--runtime-home") {
            options.runtime_home = std::filesystem::path(require_value(index));
            prune_options.runtime_home = options.runtime_home;
            report_options.runtime_home = options.runtime_home;
        } else if (arg == L"--db") {
            options.db_path = std::filesystem::path(require_value(index));
            prune_options.db_path = options.db_path;
            report_options.db_path = options.db_path;
        } else if (arg == L"--config") {
            config_path = std::filesystem::path(require_value(index));
            config_path_explicit = true;
        } else if (arg == L"--force") {
            if (!only_for(L"ingest", "ingest", "--force")) return 1;
            options.force = true;
        } else if (arg == L"--csv") {
            if (!only_for(L"ingest", "ingest", "--csv")) return 1;
            options.csv_path = std::filesystem::path(require_value(index));
        } else if (arg == L"--events") {
            if (!only_for(L"ingest", "ingest", "--events")) return 1;
            options.events_path = std::filesystem::path(require_value(index));
        } else if (arg == L"--retain-days") {
            if (!only_for(L"prune", "prune", "--retain-days")) return 1;
            std::uint32_t retain_days = 0u;
            if (!ParseUint32Option(require_value(index), retain_days)) {
                std::cerr << "Error: invalid --retain-days value.\n";
                return 1;
            }
            prune_options.retain_days = retain_days;
            retain_days_explicit = true;
        } else if (arg == L"--apply") {
            if (!only_for(L"prune", "prune", "--apply")) return 1;
            prune_options.apply = true;
        } else if (arg == L"--dry-run") {
            if (!only_for(L"prune", "prune", "--dry-run")) return 1;
            prune_options.apply = false;
        } else if (arg == L"--run") {
            if (!report_only("--run")) return 1;
            std::uint32_t run_id = 0u;
            if (!ParseUint32Option(require_value(index), run_id)) {
                std::cerr << "Error: invalid --run value.\n";
                return 1;
            }
            report_options.run_id = static_cast<std::int64_t>(run_id);
        } else if (arg == L"--session") {
            if (!report_only("--session")) return 1;
            const std::wstring value = require_value(index);
            report_options.session_start =
                std::filesystem::path(value).string();
        } else if (arg == L"--idle-seconds") {
            if (!report_only("--idle-seconds")) return 1;
            std::uint32_t idle_seconds = 0u;
            if (!ParseUint32Option(require_value(index), idle_seconds)) {
                std::cerr << "Error: invalid --idle-seconds value.\n";
                return 1;
            }
            report_options.idle_seconds = idle_seconds;
        } else if (arg == L"--load-threshold-c") {
            if (!report_only("--load-threshold-c")) return 1;
            try {
                report_options.load_threshold_c =
                    std::stod(std::wstring(require_value(index)));
            } catch (const std::exception&) {
                std::cerr << "Error: invalid --load-threshold-c value.\n";
                return 1;
            }
        } else if (arg == L"--gpu-load-threshold-c") {
            if (!report_only("--gpu-load-threshold-c")) return 1;
            try {
                report_options.gpu_load_threshold_c =
                    std::stod(std::wstring(require_value(index)));
            } catch (const std::exception&) {
                std::cerr << "Error: invalid --gpu-load-threshold-c value.\n";
                return 1;
            }
        } else if (arg == L"--p0-mhz") {
            if (!report_only("--p0-mhz")) return 1;
            try {
                const double parsed =
                    std::stod(std::wstring(require_value(index)));
                if (!std::isfinite(parsed) || parsed <= 0.0) {
                    std::cerr << "Error: invalid --p0-mhz value.\n";
                    return 1;
                }
                report_options.p0_mhz = parsed;
            } catch (const std::exception&) {
                std::cerr << "Error: invalid --p0-mhz value.\n";
                return 1;
            }
        } else if (arg == L"--out") {
            if (!report_only("--out")) return 1;
            report_options.out_path =
                std::filesystem::path(require_value(index));
        } else if (arg == L"--manifest-out") {
            if (!report_only("--manifest-out")) return 1;
            report_options.manifest_out_path =
                std::filesystem::path(require_value(index));
        } else if (arg == L"--decision-record-out") {
            if (!report_only("--decision-record-out")) return 1;
            const std::wstring value = require_value(index);
            if (value == L"auto") {
                report_options.decision_record_auto = true;
            } else {
                report_options.decision_record_out_path =
                    std::filesystem::path(value);
            }
        } else if (arg == L"--no-decision-record") {
            if (!report_only("--no-decision-record")) return 1;
            report_options.no_decision_record = true;
        } else if (arg == L"--profile") {
            if (!report_only("--profile")) return 1;
            report_options.profile =
                std::filesystem::path(require_value(index)).string();
        } else if (arg == L"--notes") {
            if (!report_only("--notes")) return 1;
            report_options.notes =
                std::filesystem::path(require_value(index)).string();
        } else if (arg == L"--hypothesis") {
            if (!report_only("--hypothesis")) return 1;
            report_options.hypothesis =
                std::filesystem::path(require_value(index)).string();
        } else if (arg == L"--decision") {
            if (!report_only("--decision")) return 1;
            report_options.decision =
                std::filesystem::path(require_value(index)).string();
        } else if (arg == L"--json") {
            if (!report_only("--json")) return 1;
            report_options.as_json = true;
        } else if (arg == L"--quiet") {
            options.quiet = true;
            prune_options.quiet = true;
            report_options.quiet = true;
        } else if (arg == L"--help" || arg == L"-h") {
            PrintAnalyzeUsage();
            return 0;
        } else {
            std::cerr << "Error: unknown analyze "
                      << std::filesystem::path(verb).string()
                      << " option.\n";
            PrintAnalyzeUsage();
            return 1;
        }
    }

    std::optional<svg_mb_control::ControlConfig> config;
    std::filesystem::path runtime_home = options.runtime_home;
    if (verb == L"prune") {
        runtime_home = prune_options.runtime_home;
    } else if (verb == L"report") {
        runtime_home = report_options.runtime_home;
    }
    if (!ResolveAnalyzeRuntimeHome(runtime_home, config_path,
                                   config_path_explicit, config)) {
        return 1;
    }
    options.runtime_home = runtime_home;
    prune_options.runtime_home = runtime_home;
    report_options.runtime_home = runtime_home;

    if (verb == L"prune") {
        if (!retain_days_explicit && config.has_value()) {
            prune_options.retain_days = config->log_retain_days;
        }
        return svg_mb_control::analyze::RunAnalyzePrune(prune_options);
    }

    if (verb == L"report") {
        return svg_mb_control::analyze::RunAnalyzeReport(report_options);
    }

    return svg_mb_control::analyze::RunAnalyzeIngest(options);
}

}  // namespace svg_mb_control::analyze
