#pragma once

// Umbrella header. The former single runtime_artifacts unit was split into
// three focused units by responsibility:
//   - runtime_paths      : RuntimeArtifactNaming, path resolvers, the local
//                           ISO-8601 formatter
//   - runtime_csv_archive: RuntimeCsvLogger (rotating CSV + manifest)
//   - runtime_event_log  : RuntimeLogEvent, AppendRuntimeEvent, event count
// This header re-exposes all three so existing includers need no change.
// New code should include the specific header it needs.

#include "runtime_csv_archive.h"
#include "runtime_event_log.h"
#include "runtime_paths.h"
