#requires -Version 5.1
<#
.SYNOPSIS
    Passive CPU-temperature-by-load comparison harness for svg-mb-control.

.DESCRIPTION
    Reads a control-loop CSV produced by svg-mb-control and bins each per-tick
    `cpu_tctl_c` reading by the *sustained* whole-system CPU load
    (`system_cpu_busy_pct`) into three regimes:

        idle : busy_pct <  -IdleMaxPct                 (default <10%)
        low  : -IdleMaxPct <= busy_pct < -LowMaxPct    (default 10-40%)
        high : busy_pct >= -LowMaxPct                   (default >=40%, "across the board")

    For each regime it reports CPU/Tctl percentiles (p50/p90/max) over the
    samples that pass a LOAD-dwell gate: a sample counts only when the load has
    been continuously in the same band, with no coverage gap larger than
    -MaxGapSeconds, for at least -DwellSeconds ending at that sample. CPU die
    temperature lags load by tens of seconds, so this excludes ramp transients
    (load just stepped up, die still cool) and makes each regime's numbers
    reflect a load-settled state rather than a moving average.

    The gate proves the LOAD was settled, not that the temperature itself has
    plateaued. Over a long sustained episode the admitted samples are dominated
    by the plateau, so p50/p90/max approximate steady state; for short bursts of
    high load the high band can still include rising-die samples, so raise
    -DwellSeconds for the high regime if you need strict thermal equilibrium.

    This is read-only with respect to the controller. It never writes fan duty,
    runtime state, the snapshot, or the analysis database. It only reads the CSV
    (with shared access, so it does not block the live writer) and appends to its
    own ledger under the experiments directory.

    Each capture is tagged with the CSV prologue's git_hash + config_sha256 (the
    "setting" identity: a config change -> controller restart -> new session with
    a new config_sha256) plus the operator-supplied -Label, -AmbientC, and
    -Notes, then appended to a persistent ledger so settings can be compared over
    time. A Markdown side-by-side comparison table is regenerated from the ledger
    on every recording run. Per-regime percentiles use the same nearest-rank
    convention as the native `analyze report` (index = round((p/100)*(n-1)),
    p100 = max), so a band's p50/p90 here equals the native analyzer's for the
    same sample set.

    WHY a load column is required: the harness needs system_cpu_busy_pct to know
    which regime each Tctl reading belongs to. The 2026-05-28 packaged build did
    not emit that column; rebuild/publish via scripts\Build-Release.ps1 so a fresh
    control-loop CSV contains it (this also closes the documented active-package
    header-drift finding).

.PARAMETER Label
    Required. Human name for the current setting/configuration being measured,
    e.g. "shipped-250ms-baseline" or "ch4-overlay+3pct". Used as the ledger key.

.PARAMETER AmbientC
    Required. Room/ambient temperature in degrees C at capture time. Cross-setting
    comparisons taken on different days are uninterpretable without it.

.PARAMETER Notes
    Optional free-text notes (workload description, subjective fan noise, etc.).

.PARAMETER Csv
    Control-loop CSV to read. Defaults to the live mirror
    release\runtime\logs\svg_mb_control_output.csv. Point at an archive under
    release\runtime\logs\archive\ to summarize a past session.

.PARAMETER IdleMaxPct
    Upper bound (exclusive) of the idle band, in system_cpu_busy_pct. Default 10.

.PARAMETER LowMaxPct
    Upper bound (exclusive) of the low band / lower bound of the high band.
    Default 40.

.PARAMETER DwellSeconds
    Minimum continuous in-band dwell, in seconds, before a sample is counted.
    Default 45.

.PARAMETER MaxGapSeconds
    Maximum allowed gap between consecutive samples inside the dwell window. A
    larger gap (sensor stall, dropped ticks, a rotated/merged archive) makes the
    dwell unprovable, so the sample is treated as indeterminate rather than
    settled. Default 5 (the control loop ticks every ~0.25 s).

.PARAMETER WindowMinutes
    If >0, only ACCUMULATE samples within the last N minutes of the CSV into the
    regime statistics. The dwell gate still uses the full preceding history, so
    the leading edge of the window is evaluated correctly. Default 0 (whole CSV).

.PARAMETER LedgerDir
    Directory for the ledger CSV and Markdown comparison. Defaults to
    release\runtime\experiments\cpu-temp-comparison (gitignored, preserved across
    publishes).

.PARAMETER Watch
    Live-monitor mode: recompute and reprint the per-band summary every
    -WatchIntervalSeconds, and do NOT touch the ledger. Use this to watch the
    bands fill while you exercise load; then run once without -Watch to record.

.PARAMETER WatchIntervalSeconds
    Refresh cadence for -Watch. Default 10.

.EXAMPLE
    .\scripts\Compare-CpuTemps.ps1 -Label "shipped-250ms-baseline" -AmbientC 22.5 `
        -Notes "first session baseline, normal desktop use"

.EXAMPLE
    .\scripts\Compare-CpuTemps.ps1 -Label live -AmbientC 22 -Watch

.NOTES
    Read-only / safe under Live Runtime Safety (AGENTS.md): no controller start /
    stop / restart, no fan writes, no runtime-state writes.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Label,
    [Parameter(Mandatory = $true)] [double]$AmbientC,
    [string]$Notes = '',
    [string]$Csv,
    [double]$IdleMaxPct = 10.0,
    [double]$LowMaxPct  = 40.0,
    [double]$DwellSeconds = 45.0,
    [double]$MaxGapSeconds = 5.0,
    [double]$WindowMinutes = 0,
    [string]$LedgerDir,
    [switch]$Watch,
    [int]$WatchIntervalSeconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture

# Ordered band identity so output column order is stable.
$BandOrder = @('idle', 'low', 'high')

# ── Helpers ──────────────────────────────────────────────────────────────────

# Read the whole file with FileShare.ReadWrite so the live controller can keep
# appending while we read (no exclusive lock, no "file in use" failure).
function Read-CsvTextShared {
    param([Parameter(Mandatory)][string]$Path)
    $fs = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object System.IO.StreamReader($fs)
        try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
    } finally { $fs.Dispose() }
}

# Re-assemble logical CSV records from physical lines: an RFC-4180 field may
# contain a quoted newline (the producer quotes any field with a comma, quote,
# CR, or LF), so a physical line with an odd number of quote characters is the
# start of a multi-line record and is joined with following lines until the
# quote count is balanced. Lines with no quotes (the overwhelmingly common case)
# pass straight through.
function ConvertTo-CsvRecords {
    param([string[]]$Lines)
    $records = New-Object System.Collections.Generic.List[string]
    $buf = $null
    foreach ($ln in $Lines) {
        if ($null -eq $buf) { $buf = $ln } else { $buf = $buf + "`n" + $ln }
        $q = if ($buf.IndexOf('"') -lt 0) { 0 } else { $buf.Length - $buf.Replace('"', '').Length }
        if (($q % 2) -eq 0) { $records.Add($buf); $buf = $null }
    }
    if ($null -ne $buf) { $records.Add($buf) }  # trailing unbalanced (truncated) record
    return $records
}

# RFC-4180 field split honoring double-quoted fields and doubled-quote escapes.
# Fast path: a record with no quote character is a plain comma split.
function Split-CsvFields {
    param([string]$Record)
    if ($Record.IndexOf('"') -lt 0) { return $Record.Split(',') }
    $fields = New-Object System.Collections.Generic.List[string]
    $sb = New-Object System.Text.StringBuilder
    $inQ = $false
    $i = 0
    $len = $Record.Length
    while ($i -lt $len) {
        $ch = $Record[$i]
        if ($inQ) {
            if ($ch -eq '"') {
                if (($i + 1) -lt $len -and $Record[$i + 1] -eq '"') { [void]$sb.Append('"'); $i++ }
                else { $inQ = $false }
            } else { [void]$sb.Append($ch) }
        } else {
            if ($ch -eq '"') { $inQ = $true }
            elseif ($ch -eq ',') { [void]$fields.Add($sb.ToString()); [void]$sb.Clear() }
            else { [void]$sb.Append($ch) }
        }
        $i++
    }
    [void]$fields.Add($sb.ToString())
    return , $fields.ToArray()
}

# Invariant-culture double parse. Returns $null on failure (so a decimal-comma
# locale never silently mis-parses a '.'-delimited field).
function ConvertTo-InvDouble {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    [double]$out = 0
    if ([double]::TryParse($Text.Trim(),
            [System.Globalization.NumberStyles]::Float,
            $Invariant, [ref]$out)) {
        return $out
    }
    return $null
}

# Invariant-culture fixed-decimal string for ledger/markdown numeric fields.
function Format-Inv {
    param($Value, [int]$Decimals = 2)
    if ($null -eq $Value) { return '' }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return '' }
    return ([double]$Value).ToString('F' + $Decimals, $Invariant)
}

# Strip newlines and escape pipes so free text is safe in a Markdown table cell.
function Format-Cell {
    param([string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return '' }
    return ($Text -replace '[\r\n]+', ' ' -replace '\|', '\|')
}

# Nearest-rank percentile on an ascending-sorted array, matching the native
# analyzer Percentile() exactly: index = round((p/100)*(n-1)) (half away from
# zero), clamped to [0, n-1]; p100 = max. See src/analyze/analyze_report_data.cpp.
function Get-Percentile {
    param([double[]]$Sorted, [double]$P)
    $n = $Sorted.Count
    if ($n -eq 0) { return [double]::NaN }
    $index = [int][math]::Round(($P / 100.0) * ($n - 1), [System.MidpointRounding]::AwayFromZero)
    if ($index -lt 0) { $index = 0 }
    if ($index -gt ($n - 1)) { $index = $n - 1 }
    return $Sorted[$index]
}

function Get-Band {
    param([double]$Busy)
    if ($Busy -lt $IdleMaxPct) { return 'idle' }
    if ($Busy -lt $LowMaxPct)  { return 'low' }
    return 'high'
}

function ConvertTo-SnapshotTime {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $t = $Text.Trim()
    $formats = @(
        'yyyy-MM-ddTHH:mm:ss',
        'yyyy-MM-ddTHH:mm:ss.fff',
        'yyyy-MM-dd HH:mm:ss')
    [datetime]$out = [datetime]::MinValue
    foreach ($f in $formats) {
        if ([datetime]::TryParseExact($t, $f, $Invariant,
                [System.Globalization.DateTimeStyles]::None, [ref]$out)) {
            return $out
        }
    }
    if ([datetime]::TryParse($t, $Invariant,
            [System.Globalization.DateTimeStyles]::None, [ref]$out)) {
        return $out
    }
    return $null
}

# ── Core: parse one CSV into per-band statistics ─────────────────────────────

function Get-Summary {
    param([string]$CsvPath)

    if (-not (Test-Path -LiteralPath $CsvPath)) {
        throw "CSV not found: $CsvPath"
    }

    $text = Read-CsvTextShared -Path $CsvPath
    $rawLines = $text -split "`r`n|`r|`n"

    # Prologue: '# key=value' lines before the header.
    $meta = @{
        session_start = ''
        git_hash      = ''
        build_version = ''
        config_sha256 = ''
        mode          = ''
    }
    $dataLines = New-Object System.Collections.Generic.List[string]
    foreach ($line in $rawLines) {
        if ($line.StartsWith('#')) {
            $kv = $line.Substring(1).TrimStart(' ')
            $eq = $kv.IndexOf('=')
            if ($eq -gt 0) {
                $k = $kv.Substring(0, $eq).Trim()
                $v = $kv.Substring($eq + 1).Trim()
                if ($meta.ContainsKey($k)) { $meta[$k] = $v }
            }
            continue
        }
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $dataLines.Add($line)
    }
    if ($dataLines.Count -eq 0) { throw "No header/data rows found in $CsvPath" }

    $records = ConvertTo-CsvRecords -Lines $dataLines
    if ($records.Count -eq 0) { throw "No header/data rows found in $CsvPath" }

    # Bind columns by NAME (archives may shift positions).
    $cols = Split-CsvFields $records[0]
    $headerWidth = $cols.Count
    $idx = @{}
    for ($c = 0; $c -lt $cols.Count; $c++) { $idx[$cols[$c].Trim()] = $c }

    foreach ($required in @('snapshot_time', 'cpu_tctl_c', 'system_cpu_busy_pct')) {
        if (-not $idx.ContainsKey($required)) {
            if ($required -eq 'system_cpu_busy_pct') {
                throw ("CSV '$CsvPath' has no 'system_cpu_busy_pct' column. It is " +
                    "from a build that does not emit the whole-system CPU load " +
                    "column (e.g. the 2026-05-28 package). Rebuild/publish via " +
                    "scripts\Build-Release.ps1 and re-run against a fresh session.")
            }
            throw "CSV '$CsvPath' has no '$required' column."
        }
    }
    $iTime = $idx['snapshot_time']
    $iTctl = $idx['cpu_tctl_c']
    $iBusy = $idx['system_cpu_busy_pct']

    # Parse data records into samples, counting drops by reason.
    $samples = New-Object System.Collections.Generic.List[object]
    $dropShort = 0; $dropTime = 0; $dropValue = 0
    for ($i = 1; $i -lt $records.Count; $i++) {
        $f = Split-CsvFields $records[$i]
        if ($f.Count -ne $headerWidth) { $dropShort++; continue }
        $t = ConvertTo-SnapshotTime $f[$iTime]
        if ($null -eq $t) { $dropTime++; continue }
        $tctl = ConvertTo-InvDouble $f[$iTctl]
        $busy = ConvertTo-InvDouble $f[$iBusy]
        if ($null -eq $tctl -or $null -eq $busy) { $dropValue++; continue }
        $samples.Add([pscustomobject]@{ T = $t; Tctl = $tctl; Busy = $busy })
    }

    # Time-ascending (a rotated/merged read might not be).
    $samples = @($samples | Sort-Object -Property T)
    $n = $samples.Count

    $times = New-Object 'double[]' $n   # seconds, for dwell math
    $bands = New-Object 'string[]' $n
    for ($i = 0; $i -lt $n; $i++) {
        $times[$i] = $samples[$i].T.Ticks / [double]10000000
        $bands[$i] = Get-Band $samples[$i].Busy
    }

    # Reporting window applies to ACCUMULATION only; the dwell scan always uses
    # full preceding history so the window's leading edge evaluates correctly.
    $cutoff = [double]::NegativeInfinity
    if ($WindowMinutes -gt 0 -and $n -gt 0) {
        $cutoff = $times[$n - 1] - ($WindowMinutes * 60.0)
    }

    $bandTctl = @{}; $bandBusy = @{}
    $cInBand = @{}; $cRamp = @{}; $cIndet = @{}
    foreach ($b in $BandOrder) {
        $bandTctl[$b] = New-Object System.Collections.Generic.List[double]
        $bandBusy[$b] = New-Object System.Collections.Generic.List[double]
        $cInBand[$b] = 0; $cRamp[$b] = 0; $cIndet[$b] = 0
    }

    for ($i = 0; $i -lt $n; $i++) {
        if ($times[$i] -lt $cutoff) { continue }   # outside reporting window
        $b = $bands[$i]
        $cInBand[$b]++
        $windowStart = $times[$i] - $DwellSeconds

        # Backward scan: require continuous, same-band coverage with no gap >
        # MaxGapSeconds, reaching a sample at/before windowStart.
        $prevT = $times[$i]
        $reachedStart = $false
        $reason = 'settled'
        $j = $i - 1
        while ($j -ge 0) {
            if (($prevT - $times[$j]) -gt $MaxGapSeconds) { $reason = 'indeterminate'; break }
            if ($times[$j] -le $windowStart) { $reachedStart = $true; break }
            if ($bands[$j] -ne $b) { $reason = 'ramp'; break }
            $prevT = $times[$j]
            $j--
        }
        if ($reason -eq 'settled' -and -not $reachedStart) { $reason = 'indeterminate' }

        if ($reason -eq 'settled') {
            $bandTctl[$b].Add($samples[$i].Tctl)
            $bandBusy[$b].Add($samples[$i].Busy)
        } elseif ($reason -eq 'ramp') {
            $cRamp[$b]++
        } else {
            $cIndet[$b]++
        }
    }

    $bandStats = @{}
    foreach ($b in $BandOrder) {
        $vals = @($bandTctl[$b] | Sort-Object)
        $cnt = $vals.Count
        $busyVals = @($bandBusy[$b])
        $bandStats[$b] = [pscustomobject]@{
            band      = $b
            n         = $cnt
            n_ramp    = [int]$cRamp[$b]
            n_indet   = [int]$cIndet[$b]
            n_inband  = [int]$cInBand[$b]
            p50       = if ($cnt) { Get-Percentile $vals 50 } else { [double]::NaN }
            p90       = if ($cnt) { Get-Percentile $vals 90 } else { [double]::NaN }
            max       = if ($cnt) { $vals[$cnt - 1] } else { [double]::NaN }
            min       = if ($cnt) { $vals[0] } else { [double]::NaN }
            mean      = if ($cnt) { ($vals | Measure-Object -Average).Average } else { [double]::NaN }
            busy_mean = if ($busyVals.Count) { ($busyVals | Measure-Object -Average).Average } else { [double]::NaN }
        }
    }

    $first = if ($n) { $samples[0].T } else { $null }
    $last  = if ($n) { $samples[$n - 1].T } else { $null }

    return [pscustomobject]@{
        CsvPath     = $CsvPath
        Meta        = $meta
        BandStats   = $bandStats
        TotalRows   = $n
        DropShort   = $dropShort
        DropTime    = $dropTime
        DropValue   = $dropValue
        FirstTime   = $first
        LastTime    = $last
        SpanMinutes = if ($first -and $last) { ($last - $first).TotalMinutes } else { 0 }
    }
}

# ── Rendering ────────────────────────────────────────────────────────────────

function Show-Summary {
    param($Summary)
    $m = $Summary.Meta
    $cfgShort = if ($m.config_sha256) { $m.config_sha256.Substring(0, [math]::Min(12, $m.config_sha256.Length)) } else { '' }
    Write-Host ''
    Write-Host "CPU temp by sustained load" -ForegroundColor Cyan
    Write-Host ("  source   : {0}" -f $Summary.CsvPath)
    Write-Host ("  setting  : Label='{0}'  git={1}  cfg={2}  build={3}" -f $Label, $m.git_hash, $cfgShort, $m.build_version)
    Write-Host ("  session  : {0}   span={1} min   samples={2}   ambient={3} C" -f `
        $m.session_start, (Format-Inv $Summary.SpanMinutes 1), $Summary.TotalRows, (Format-Inv $AmbientC 1))
    Write-Host ("  bands    : idle<{0}%  low<{1}%  high>={1}%   dwell>={2}s  maxgap={3}s" -f `
        (Format-Inv $IdleMaxPct 0), (Format-Inv $LowMaxPct 0), (Format-Inv $DwellSeconds 0), (Format-Inv $MaxGapSeconds 0))
    $dropTotal = $Summary.DropShort + $Summary.DropTime + $Summary.DropValue
    if ($dropTotal -gt 0) {
        Write-Host ("  dropped  : {0} rows (short={1} bad-time={2} blank/NaN={3})" -f `
            $dropTotal, $Summary.DropShort, $Summary.DropTime, $Summary.DropValue) -ForegroundColor DarkYellow
    }

    $rows = foreach ($b in $BandOrder) {
        $s = $Summary.BandStats[$b]
        [pscustomobject]@{
            Regime         = $b
            'n(settled)'   = $s.n
            'ramp'         = $s.n_ramp
            'no-dwell'     = $s.n_indet
            'busy~%'       = if ($s.n) { Format-Inv $s.busy_mean 1 } else { '-' }
            'Tctl p50'     = if ($s.n) { Format-Inv $s.p50 2 } else { '(no data)' }
            'Tctl p90'     = if ($s.n) { Format-Inv $s.p90 2 } else { '-' }
            'Tctl max'     = if ($s.n) { Format-Inv $s.max 2 } else { '-' }
        }
    }
    $rows | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host "  legend: settled=load-settled samples; ramp=band changed within dwell; no-dwell=insufficient/gapped history" -ForegroundColor DarkGray

    if ($WindowMinutes -gt 0 -and $Summary.SpanMinutes -lt $WindowMinutes) {
        Write-Host ("  note: requested window {0} min exceeds the CSV span {1} min — only {1} min was available." -f `
            (Format-Inv $WindowMinutes 0), (Format-Inv $Summary.SpanMinutes 1)) -ForegroundColor DarkYellow
    }

    $empty = @($BandOrder | Where-Object { $Summary.BandStats[$_].n -eq 0 })
    if ($empty.Count -gt 0) {
        Write-Host ("  note: no settled samples yet for: {0}." -f ($empty -join ', ')) -ForegroundColor DarkYellow
        Write-Host  "        Passive mode fills a band only once that load is sustained >= dwell." -ForegroundColor DarkYellow
        Write-Host  "        Exercise that workload and re-run, or check the load is not hovering on a band boundary." -ForegroundColor DarkYellow
    }
}

# ── Ledger + Markdown ────────────────────────────────────────────────────────

function Write-Ledger {
    param($Summary, [string]$LedgerCsv)
    $dir = Split-Path -Parent $LedgerCsv
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $now = Get-Date
    $runId = $now.ToString('yyyyMMdd_HHmmss_fff', $Invariant)
    $capturedAt = $now.ToString('yyyy-MM-ddTHH:mm:ss', $Invariant)
    $m = $Summary.Meta
    $newRows = foreach ($b in $BandOrder) {
        $s = $Summary.BandStats[$b]
        $lo = if ($b -eq 'idle') { 0 } elseif ($b -eq 'low') { $IdleMaxPct } else { $LowMaxPct }
        $hi = if ($b -eq 'idle') { $IdleMaxPct } elseif ($b -eq 'low') { $LowMaxPct } else { 100 }
        [pscustomobject][ordered]@{
            run_id            = $runId
            captured_at       = $capturedAt
            label             = $Label
            ambient_c         = Format-Inv $AmbientC 2
            notes             = $Notes
            band              = $b
            band_busy_lo      = Format-Inv $lo 0
            band_busy_hi      = Format-Inv $hi 0
            dwell_s           = Format-Inv $DwellSeconds 0
            max_gap_s         = Format-Inv $MaxGapSeconds 0
            n                 = $s.n
            n_ramp            = $s.n_ramp
            n_indeterminate   = $s.n_indet
            tctl_p50          = Format-Inv $s.p50 2
            tctl_p90          = Format-Inv $s.p90 2
            tctl_max          = Format-Inv $s.max 2
            tctl_min          = Format-Inv $s.min 2
            tctl_mean         = Format-Inv $s.mean 2
            busy_mean         = Format-Inv $s.busy_mean 2
            csv_session_start = $m.session_start
            git_hash          = $m.git_hash
            build_version     = $m.build_version
            config_sha256     = $m.config_sha256
            window_min        = Format-Inv $WindowMinutes 0
            dropped_rows      = $Summary.DropShort + $Summary.DropTime + $Summary.DropValue
            source_csv        = $Summary.CsvPath
        }
    }

    # Guard against silent schema drift: if a ledger from a different script
    # version exists, Export-Csv -Append would silently drop new columns. Fail
    # loudly with an actionable message instead.
    $expected = @($newRows)[0].PSObject.Properties.Name
    if (Test-Path -LiteralPath $LedgerCsv) {
        $firstLine = (Read-CsvTextShared $LedgerCsv) -split "`r`n|`r|`n" |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -First 1
        if ($firstLine) {
            $existing = @(Split-CsvFields $firstLine | ForEach-Object { $_.Trim('"') })
            if (($existing -join '|') -ne ($expected -join '|')) {
                throw ("Ledger schema mismatch at $LedgerCsv. The existing ledger has a " +
                    "different column set than this script version. Rename the old ledger " +
                    "(e.g. ledger.pre-$($now.ToString('yyyyMMdd', $Invariant)).csv) and re-run " +
                    "to start a fresh ledger.`n  existing: $($existing -join ',')`n  expected: $($expected -join ',')")
            }
        }
    }

    $newRows | Export-Csv -LiteralPath $LedgerCsv -Append -NoTypeInformation -Encoding UTF8
    return $runId
}

function Write-ComparisonMarkdown {
    param([string]$LedgerCsv, [string]$MarkdownPath)
    if (-not (Test-Path -LiteralPath $LedgerCsv)) { return }
    $all = @(Import-Csv -LiteralPath $LedgerCsv)
    if ($all.Count -eq 0) { return }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('# CPU temperature by sustained load — setting comparison')
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine(('_Generated {0} from `{1}`._' -f `
        ((Get-Date).ToString('yyyy-MM-dd HH:mm', $Invariant)), (Split-Path -Leaf $LedgerCsv)))
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('Bands are by `system_cpu_busy_pct`; each cell is `cpu_tctl_c` (C) over')
    [void]$sb.AppendLine('load-settled samples (continuous in-band for the dwell window). `n` is the')
    [void]$sb.AppendLine('settled sample count; an empty band (`-`) means that load was not sustained')
    [void]$sb.AppendLine('long enough yet.')
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('| Captured | Label | Amb C | git | cfg | idle p50/p90/max (n) | low p50/p90/max (n) | high p50/p90/max (n) | Notes |')
    [void]$sb.AppendLine('|---|---|---|---|---|---|---|---|---|')

    $byRun = $all | Group-Object run_id | Sort-Object { $_.Group[0].captured_at }
    foreach ($g in $byRun) {
        $rows = $g.Group
        $any = $rows[0]
        $cell = {
            param($band)
            $r = $rows | Where-Object { $_.band -eq $band } | Select-Object -First 1
            if ($null -eq $r -or [int]$r.n -eq 0) { return '-' }
            return ('{0}/{1}/{2} ({3})' -f $r.tctl_p50, $r.tctl_p90, $r.tctl_max, $r.n)
        }
        $cfgShort = if ($any.config_sha256) { $any.config_sha256.Substring(0, [math]::Min(8, $any.config_sha256.Length)) } else { '' }
        [void]$sb.AppendLine(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |' -f `
            $any.captured_at, (Format-Cell $any.label), $any.ambient_c, $any.git_hash, $cfgShort,
            (& $cell 'idle'), (& $cell 'low'), (& $cell 'high'), (Format-Cell $any.notes)))
    }

    Set-Content -LiteralPath $MarkdownPath -Value $sb.ToString() -Encoding UTF8
}

# ── Main ─────────────────────────────────────────────────────────────────────

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Csv)) {
    $Csv = Join-Path $repoRoot 'release\runtime\logs\svg_mb_control_output.csv'
}
if ([string]::IsNullOrWhiteSpace($LedgerDir)) {
    $LedgerDir = Join-Path $repoRoot 'release\runtime\experiments\cpu-temp-comparison'
}
$LedgerCsv = Join-Path $LedgerDir 'ledger.csv'
$MarkdownPath = Join-Path $LedgerDir 'comparison.md'

if ($IdleMaxPct -ge $LowMaxPct) {
    throw "-IdleMaxPct ($IdleMaxPct) must be < -LowMaxPct ($LowMaxPct)."
}

if ($Watch) {
    Write-Host "Live monitor (Ctrl+C to stop) — ledger is NOT written in -Watch mode." -ForegroundColor Yellow
    while ($true) {
        try {
            $summary = Get-Summary -CsvPath $Csv
            Clear-Host
            Show-Summary -Summary $summary
        } catch {
            Write-Host ("read error: {0}" -f $_.Exception.Message) -ForegroundColor Red
        }
        Start-Sleep -Seconds $WatchIntervalSeconds
    }
    return
}

$summary = Get-Summary -CsvPath $Csv
Show-Summary -Summary $summary
$runId = Write-Ledger -Summary $summary -LedgerCsv $LedgerCsv
Write-ComparisonMarkdown -LedgerCsv $LedgerCsv -MarkdownPath $MarkdownPath

Write-Host ''
Write-Host ("Recorded run {0} to:" -f $runId) -ForegroundColor Green
Write-Host ("  ledger : {0}" -f $LedgerCsv)
Write-Host ("  table  : {0}" -f $MarkdownPath)
