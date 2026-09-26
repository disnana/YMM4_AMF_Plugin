[CmdletBinding()]
param(
    [ValidateSet('OutputWait', 'InputRecycling')][string]$Experiment = 'OutputWait',
    [switch]$EnableOutputWaitForInputComparison,
    [ValidateRange(2, 7680)][int]$Width = 1920,
    [ValidateRange(2, 4320)][int]$Height = 1080,
    [ValidateRange(120, 36000)][int]$Frames = 900,
    [ValidateRange(2, 10)][int]$Pairs = 4,
    [ValidateSet('h264', 'hevc')][string[]]$Codecs = @('h264', 'hevc'),
    [ValidateSet(4, 6, 8, 16, 32, 64, 128)][int[]]$PoolSizes = @(6),
    [ValidateSet('speed', 'balanced', 'quality')][string]$Quality = 'balanced',
    [ValidateRange(100, 200000)][int]$BitrateKbps = 12000,
    [ValidateRange(100, 200000)][int]$MaxBitrateKbps = 14400,
    [switch]$Profile,
    [switch]$SkipValidation
)

$ErrorActionPreference = 'Stop'
if (($Width % 2) -or ($Height % 2)) { throw 'Dimensions must be even.' }
if ($EnableOutputWaitForInputComparison -and $Experiment -ne 'InputRecycling') { throw 'The fixed wait option is only for InputRecycling comparisons.' }
if ($MaxBitrateKbps -lt $BitrateKbps) { throw 'MaxBitrateKbps must not be below BitrateKbps.' }
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$bench = Join-Path $repositoryRoot 'artifacts/bin/RadeonBench.exe'
$native = Join-Path $repositoryRoot 'artifacts/bin/AmfNative.dll'
if (-not (Test-Path -LiteralPath $bench)) { throw 'Run scripts/Build.ps1 first.' }
if (-not $SkipValidation -and (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue) -or -not (Get-Command ffprobe -ErrorAction SilentlyContinue))) {
    throw 'Validation requires ffmpeg and ffprobe.'
}
$prefix = if ($Experiment -eq 'InputRecycling') { 'input-recycling-' } else { 'output-wait-' }
$runRoot = Join-Path $repositoryRoot ('artifacts/runs/' + $prefix + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$rows = [Collections.Generic.List[object]]::new()

function Invoke-Case([string]$Codec, [int]$Pool, [bool]$Optimized, [int]$Count, [string]$Name) {
    $caseRoot = Join-Path $runRoot $Name
    New-Item -ItemType Directory -Path $caseRoot | Out-Null
    $output = Join-Path $caseRoot 'output.mp4'
    $result = Join-Path $caseRoot 'run.json'
    $startInfo = [Diagnostics.ProcessStartInfo]::new($bench)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $arguments = @('run', '--output', $output, '--result', $result, '--codec', $Codec,
        '--width', "$Width", '--height', "$Height", '--fps', '60', '--frames', "$Count",
        '--bitrate-kbps', "$BitrateKbps", '--max-bitrate-kbps', "$MaxBitrateKbps", '--quality', $Quality,
        '--rate-control', 'vbr', '--pool-size', "$Pool", '--audio', '--no-debug-log')
    $waitRequested = ($Experiment -eq 'OutputWait' -and $Optimized) -or $EnableOutputWaitForInputComparison
    $recycleRequested = $Experiment -eq 'InputRecycling' -and $Optimized
    if ($waitRequested) { $arguments += '--optimize-output-wait' }
    if ($recycleRequested) { $arguments += '--recycle-input' }
    if ($Profile) { $arguments += '--profile' }
    foreach ($argument in $arguments) { $startInfo.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(120)
        while (-not $process.WaitForExit(1000)) {
            if ([DateTime]::UtcNow -gt $deadline) {
                $process.Kill($true)
                $process.WaitForExit()
                throw "Benchmark timed out: $Name"
            }
        }
        if ($process.ExitCode -ne 0) { throw "Benchmark failed: $Name; see $result" }
        $cpuMs = $process.TotalProcessorTime.TotalMilliseconds
        $peakWorkingSet = $process.PeakWorkingSet64
    }
    finally { $process.Dispose() }
    $run = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    if ($run.status -ne 'passed' -or $run.metrics.accepted_frames -ne $Count) { throw "Incomplete case: $Name" }
    if ($run.output_wait.requested -ne $waitRequested) { throw 'Requested wait mode mismatch.' }
    if ($run.input_recycling.requested -ne $recycleRequested -or $run.input_recycling.invalid_events -ne 0) { throw 'Requested input recycling mismatch.' }
    if (-not $SkipValidation) {
        & (Join-Path $PSScriptRoot 'Validate-Output.ps1') -Output $output -Result $result `
            -Codec $Codec -Width $Width -Height $Height -Fps 60 -ExpectedFrames $Count -ExpectAudio | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Output validation failed: $Name" }
    }
    $row = [pscustomobject]@{
        case = $Name; codec = $Codec; pool = $Pool; optimized = $Optimized; frames = $Count
        wall_ms = $run.metrics.export_wall_ms; fps = $run.metrics.completed_fps
        process_cpu_ms = $cpuMs; process_peak_working_set_bytes = $peakWorkingSet
        effective_mode = $run.output_wait.effective_mode; reason = $run.output_wait.reason
        early_poll_waits = $run.output_wait.early_poll_waits
        high_resolution_poll_waits = $run.output_wait.high_resolution_poll_waits
        input_recycling = $run.input_recycling
        validation = if ($SkipValidation) { 'not_run' } else { 'decode_order_metadata_duration_passed' }
    }
    Write-Host "$Name : $($row.fps) fps, $($row.wall_ms) ms, $($row.reason)"
    return $row
}

foreach ($codec in $Codecs) {
    foreach ($pool in $PoolSizes) {
        # Separate warm-up of both modes; then alternate paired AB / BA order.
        foreach ($optimized in @($false, $true)) {
            Invoke-Case $codec $pool $optimized 120 "$codec-p$pool-warmup-$optimized" | Out-Null
        }
        for ($pair = 0; $pair -lt $Pairs; $pair++) {
            $order = if (($pair % 2) -eq 0) { @($false, $true) } else { @($true, $false) }
            foreach ($optimized in $order) {
                $rows.Add((Invoke-Case $codec $pool $optimized $Frames "$codec-p$pool-pair$pair-$optimized"))
            }
        }
    }
}
function Median($Values) {
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
}
$comparisons = foreach ($codec in $Codecs) { foreach ($pool in $PoolSizes) {
    $off = @($rows | Where-Object { $_.codec -eq $codec -and $_.pool -eq $pool -and -not $_.optimized })
    $on = @($rows | Where-Object { $_.codec -eq $codec -and $_.pool -eq $pool -and $_.optimized })
    $offMs = Median $off.wall_ms
    $onMs = Median $on.wall_ms
    [pscustomobject]@{
        codec = $codec; pool = $pool; off_median_ms = $offMs; on_median_ms = $onMs
        off_median_fps = Median $off.fps; on_median_fps = Median $on.fps
        speed_ratio = $offMs / $onMs
        off_min_ms = ($off.wall_ms | Measure-Object -Minimum).Minimum
        off_max_ms = ($off.wall_ms | Measure-Object -Maximum).Maximum
        on_min_ms = ($on.wall_ms | Measure-Object -Minimum).Minimum
        on_max_ms = ($on.wall_ms | Measure-Object -Maximum).Maximum
    }
} }
$summary = [ordered]@{
    schema_version = 1; created = (Get-Date).ToString('o'); width = $Width; height = $Height; fps = 60
    frames = $Frames; pairs = $Pairs; profiling = $Profile.IsPresent; validated = -not $SkipValidation
    experiment = $Experiment; fixed_output_wait = $EnableOutputWaitForInputComparison.IsPresent
    quality = $Quality; rate_control = 'vbr'; bitrate_kbps = $BitrateKbps; max_bitrate_kbps = $MaxBitrateKbps
    benchmark_sha256 = (Get-FileHash -LiteralPath $bench).Hash; native_sha256 = (Get-FileHash -LiteralPath $native).Hash
    os = [Environment]::OSVersion.VersionString
    notes = @('Synthetic frame generation/upload + AMF + AAC + MP4, not YMM4 end-to-end.',
        'Wall time excludes encoder initialization; process CPU time includes initialization/cleanup.',
        'Separate warmups excluded; paired AB/BA order; validation outside measured export.',
        'Peak working set may be unavailable/null after process exit; GPU memory is not measured.',
        'Color metadata and A/V duration are validated, not perceptual quality or audible synchronization.')
    comparisons = @($comparisons); runs = @($rows.ToArray())
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $runRoot 'summary.json') -Encoding utf8
$comparisons | Format-Table | Out-Host
Write-Host "Results: $runRoot"
