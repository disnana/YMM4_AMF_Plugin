[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Config
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$configPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Config))
$settings = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
if ($settings.schema_version -ne 1) { throw 'Only benchmark schema_version 1 is supported.' }
$allowedKeys = @(
    'schema_version', 'suite', 'seed', 'fixture', 'width', 'height', 'fps', 'frames',
    'warmup_frames', 'warmup_separate_session', 'repeats', 'paths', 'pool_sizes', 'codec',
    'rate_control', 'target_bitrate_bps', 'max_bitrate_bps', 'quality_intent', 'output_mode',
    'telemetry', 'validate_output', 'allow_software_fallback'
)
foreach ($property in $settings.PSObject.Properties.Name) {
    if ($property -notin $allowedKeys) { throw "Unknown benchmark config key: $property" }
}
$unsupportedPath = @($settings.paths | Where-Object { $_ -ne 'copy_bgra_efc' })
if (@($settings.paths).Count -eq 0) { throw 'paths must contain at least one benchmark path.' }
if ($unsupportedPath.Count -gt 0) { throw "Unsupported paths: $($unsupportedPath -join ', '). Only copy_bgra_efc is implemented." }
if ($settings.fixture -ne 'moving-markers-sdr') { throw 'Only the moving-markers-sdr fixture is implemented.' }
if ($settings.fps.den -ne 1) { throw 'RadeonBench currently supports only an integer frame rate (fps.den = 1).' }
if ([int]$settings.fps.num -lt 1) { throw 'fps.num must be positive.' }
if ([int]$settings.width -lt 2 -or [int]$settings.height -lt 2 -or ($settings.width % 2) -ne 0 -or ($settings.height % 2) -ne 0) { throw 'width and height must be positive even values.' }
if ([int]$settings.frames -lt 1 -or [int]$settings.repeats -lt 1) { throw 'frames and repeats must be positive.' }
$invalidPoolSizes = @($settings.pool_sizes | Where-Object { $_ -notin @(4, 6, 8) })
if (@($settings.pool_sizes).Count -eq 0 -or $invalidPoolSizes.Count -gt 0) { throw 'pool_sizes must contain only 4, 6, or 8.' }
if ($settings.codec -notin @('h264', 'hevc')) { throw 'codec must be h264 or hevc.' }
if ($settings.rate_control -notin @('cbr', 'vbr')) { throw 'rate_control must be cbr or vbr.' }
if ($settings.quality_intent -notin @('speed', 'balanced', 'quality')) { throw 'quality_intent is unsupported.' }
if ($settings.output_mode -ne 'mp4') { throw 'Only MP4 output is implemented.' }
if ($settings.telemetry -ne 'summary') { throw 'Only summary telemetry is implemented.' }
if ($settings.allow_software_fallback) { throw 'Software fallback is intentionally unsupported.' }
if ($settings.warmup_frames -gt 0 -and -not $settings.warmup_separate_session) { throw 'Warm-up must use a separate session.' }

& (Join-Path $PSScriptRoot 'Build.ps1') -Configuration Release
$bench = Join-Path $repositoryRoot 'artifacts\bin\RadeonBench.exe'
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runRoot = Join-Path $repositoryRoot "artifacts\runs\benchmark-$timestamp"
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$rows = @()
$environment = $null
$ffprobe = if ($settings.validate_output) { Get-Command ffprobe -ErrorAction SilentlyContinue } else { $null }
$ffmpeg = if ($settings.validate_output) { Get-Command ffmpeg -ErrorAction SilentlyContinue } else { $null }
if ($settings.validate_output -and (-not $ffprobe -or -not $ffmpeg)) {
    throw 'validate_output=true requires ffmpeg and ffprobe on PATH.'
}

foreach ($poolSize in $settings.pool_sizes) {
    if ($settings.warmup_frames -gt 0) {
        $warmupRoot = Join-Path $runRoot "warmup-pool$poolSize"
        New-Item -ItemType Directory -Path $warmupRoot -Force | Out-Null
        & $bench run --output (Join-Path $warmupRoot 'output.mp4') --result (Join-Path $warmupRoot 'run.json') `
            --codec $settings.codec --width $settings.width --height $settings.height --fps $settings.fps.num `
            --frames $settings.warmup_frames --bitrate-kbps ([int]($settings.target_bitrate_bps / 1000)) `
            --max-bitrate-kbps ([int]($settings.max_bitrate_bps / 1000)) --rate-control $settings.rate_control `
            --quality $settings.quality_intent --pool-size $poolSize
        if ($LASTEXITCODE -ne 0) { throw "Warm-up failed for pool size $poolSize." }
    }
    for ($repeat = 1; $repeat -le $settings.repeats; $repeat++) {
        $caseRoot = Join-Path $runRoot "copy_bgra_efc-pool$poolSize-run$repeat"
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        $output = Join-Path $caseRoot 'output.mp4'
        $result = Join-Path $caseRoot 'run.json'
        & $bench run --output $output --result $result --codec $settings.codec --width $settings.width --height $settings.height --fps $settings.fps.num `
            --frames $settings.frames --bitrate-kbps ([int]($settings.target_bitrate_bps / 1000)) `
            --max-bitrate-kbps ([int]($settings.max_bitrate_bps / 1000)) --rate-control $settings.rate_control `
            --quality $settings.quality_intent --pool-size $poolSize
        if ($LASTEXITCODE -ne 0) { throw "Benchmark failed. See $result" }
        if ($settings.validate_output) {
            & (Join-Path $PSScriptRoot 'Validate-Output.ps1') -Output $output -Result $result `
                -Codec $settings.codec -Width $settings.width -Height $settings.height `
                -Fps $settings.fps.num -ExpectedFrames $settings.frames
            if ($LASTEXITCODE -ne 0) { throw "Output validation failed. See $result" }
        }
        $run = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        $environment = $run.environment
        $rows += [pscustomobject]@{
            path = 'copy_bgra_efc'
            pool_size = $poolSize
            repeat = $repeat
            export_wall_ms = $run.metrics.export_wall_ms
            accepted_frames = $run.metrics.accepted_frames
            completed_fps = $run.metrics.completed_fps
            status = $run.status
        }
    }
}

$rows | Export-Csv -LiteralPath (Join-Path $runRoot 'comparison.csv') -NoTypeInformation -Encoding utf8
$cases = @(
    $rows | Group-Object path, pool_size | ForEach-Object {
        $fps = @($_.Group.completed_fps | ForEach-Object { [double]$_ } | Sort-Object)
        $wallMs = @($_.Group.export_wall_ms | ForEach-Object { [double]$_ } | Sort-Object)
        $middle = [Math]::Floor($fps.Count / 2)
        $medianFps = if (($fps.Count % 2) -eq 0) { ($fps[$middle - 1] + $fps[$middle]) / 2.0 } else { $fps[$middle] }
        $medianWallMs = if (($wallMs.Count % 2) -eq 0) { ($wallMs[$middle - 1] + $wallMs[$middle]) / 2.0 } else { $wallMs[$middle] }
        [ordered]@{
            path = $_.Group[0].path
            pool_size = [int]$_.Group[0].pool_size
            runs = $fps.Count
            completed_fps = [ordered]@{
                median = [Math]::Round($medianFps, 3)
                mean = [Math]::Round(($fps | Measure-Object -Average).Average, 3)
                min = [Math]::Round($fps[0], 3)
                max = [Math]::Round($fps[-1], 3)
            }
            export_wall_ms = [ordered]@{
                median = [Math]::Round($medianWallMs, 3)
                mean = [Math]::Round(($wallMs | Measure-Object -Average).Average, 3)
                min = [Math]::Round($wallMs[0], 3)
                max = [Math]::Round($wallMs[-1], 3)
            }
        }
    }
)
$summary = [ordered]@{
    schema_version = 1
    suite = $settings.suite
    fixture = $settings.fixture
    environment = $environment
    config = [ordered]@{
        width = $settings.width
        height = $settings.height
        fps = $settings.fps
        frames = $settings.frames
        repeats = $settings.repeats
        codec = $settings.codec
        validate_output = $settings.validate_output
    }
    cases = $cases
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $runRoot 'summary.json') -Encoding utf8
Write-Host "Benchmark results: $runRoot"
