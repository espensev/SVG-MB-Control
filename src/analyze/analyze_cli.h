#pragma once

namespace svg_mb_control::analyze {

// Parses and dispatches the `analyze` verb (ingest / prune / report) from the
// process argv. argv[1] is expected to be "analyze"; subcommand and options
// follow. Returns the process exit code (0 on success, non-zero on usage or
// runtime error). Read-only with respect to control runtime state except for
// the sqlite ingest database it manages.
int RunAnalyzeCommand(int argc, wchar_t** argv);

}  // namespace svg_mb_control::analyze
