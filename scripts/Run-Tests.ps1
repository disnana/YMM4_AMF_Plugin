[CmdletBinding()]
param(
    [ValidateSet('Unit', 'GpuSmoke', 'All')]
    [string]$Suite = 'Unit',
    [string]$Ymm4Directory
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'Build.ps1') -Configuration Release
$bench = Join-Path $repositoryRoot 'artifacts\bin\RadeonBench.exe'

if (-not $Ymm4Directory) { $Ymm4Directory = Join-Path $repositoryRoot 'YukkuriMovieMaker_v4_Lite' }
if (-not (Test-Path -LiteralPath (Join-Path $Ymm4Directory 'YukkuriMovieMaker.Plugin.dll'))) {
    throw 'Managed diagnostic tests require -Ymm4Directory pointing to YMM4 build references.'
}
$normalizedYmm4 = [IO.Path]::GetFullPath($Ymm4Directory) + [IO.Path]::DirectorySeparatorChar
$testProject = Join-Path $repositoryRoot 'tests\AMFVideoWriterPlugin.Tests\AMFVideoWriterPlugin.Tests.csproj'
& dotnet build $testProject -c Release -p:Platform=x64 "-p:YMM4DirPath=$normalizedYmm4"
if ($LASTEXITCODE -ne 0) { throw 'Managed diagnostic test build failed.' }
$testAssembly = Join-Path $repositoryRoot 'tests\AMFVideoWriterPlugin.Tests\bin\x64\Release\net10.0-windows10.0.19041.0\AMFVideoWriterPlugin.Tests.dll'

if ($Suite -in @('Unit', 'All')) {
    $parseFailures = @()
    $parseScripts = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File) +
        @(Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'tests/MfAmfPathBench') -Filter '*.ps1' -File)
    foreach ($script in $parseScripts) {
        $tokens = $null
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$errors)
        foreach ($parseError in $errors) {
            $parseFailures += "$($script.Name):$($parseError.Extent.StartLineNumber): $($parseError.Message)"
        }
    }
    if ($parseFailures.Count -gt 0) {
        throw "PowerShell parse checks failed:`n$($parseFailures -join "`n")"
    }
    & $bench 2>$null
    if ($LASTEXITCODE -eq 0) { throw 'CLI unexpectedly accepted missing arguments.' }
    & $bench run --pool-size 3 2>$null
    if ($LASTEXITCODE -eq 0) { throw 'CLI unexpectedly accepted an invalid pool size.' }
    Write-Host 'PowerShell parse and CLI contract checks: passed'
    & (Join-Path $repositoryRoot 'tests/Test-ExportQuality.ps1')
    & $bench profile-self-test
    if ($LASTEXITCODE -ne 0) { throw 'Native profiling counter checks failed.' }
    & $bench output-wait-self-test
    if ($LASTEXITCODE -ne 0) { throw 'Native output-wait capability/rollback checks failed.' }
    & $bench input-recycling-self-test
    if ($LASTEXITCODE -ne 0) { throw 'Native input-recycling lifetime/bounds checks failed.' }
    & $bench pipeline-self-test
    if ($LASTEXITCODE -ne 0) { throw 'Native submission queue ownership/drain/abort checks failed.' }
    $testResults = Join-Path $repositoryRoot ('artifacts\runs\diagnostic-tests-' + [Guid]::NewGuid().ToString('N'))
    & dotnet $testAssembly $testResults
    if ($LASTEXITCODE -ne 0) { throw 'Managed diagnostic tests failed.' }
}

if ($Suite -in @('GpuSmoke', 'All')) {
    $runRoot = Join-Path $repositoryRoot 'artifacts\runs\gpu-smoke'
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
    $ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if (-not $ffprobe -or -not $ffmpeg) {
        throw 'GpuSmoke requires ffmpeg and ffprobe on PATH so encoded output is not accepted without validation.'
    }
    & (Join-Path $repositoryRoot 'tests/Test-ExportQuality.ps1') -Integration
    $defaultsRoot = Join-Path $runRoot ('defaults-' + [Guid]::NewGuid().ToString('N'))
    & dotnet $testAssembly --gpu-defaults $defaultsRoot (Join-Path $repositoryRoot 'artifacts\bin\AmfNative.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Default optimized writer GPU checks failed.' }
    foreach ($codec in @('h264', 'hevc')) {
        foreach ($mode in @('off', 'profile')) {
            $caseRoot = Join-Path $defaultsRoot "$codec-$mode-cpu-read-wait-on-recycle-off"
            & (Join-Path $PSScriptRoot 'Validate-Output.ps1') -Output (Join-Path $caseRoot 'output.mp4') `
                -Result (Join-Path $caseRoot 'run.json') -Codec $codec -Width 640 -Height 360 -Fps 60 -ExpectedFrames 120 -ExpectAudio
            if ($LASTEXITCODE -ne 0) { throw 'Default optimized output validation failed.' }
        }
    }
    foreach ($codec in @('h264', 'hevc')) {
      foreach ($profiling in @($false, $true)) {
       foreach ($optimizeWait in @($false, $true)) {
        foreach ($recycleInput in @($false, $true)) {
        $caseName = if ($profiling) { "$codec-profile" } else { $codec }
        $caseName += if ($optimizeWait) { '-wait-on' } else { '-wait-off' }
        $caseName += if ($recycleInput) { '-recycle-on' } else { '-recycle-off' }
        $caseRoot = Join-Path $runRoot $caseName
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        $output = Join-Path $caseRoot 'output.mp4'
        $result = Join-Path $caseRoot 'run.json'
        [string[]]$profileArguments = @()
        if ($profiling) { $profileArguments += @('--profile', '--no-debug-log') }
        if ($optimizeWait) { $profileArguments += '--optimize-output-wait' }
        if ($recycleInput) { $profileArguments += '--recycle-input' }
        & $bench run --output $output --result $result --codec $codec --width 640 --height 360 --fps 60 --frames 120 --bitrate-kbps 4000 --pool-size 4 --audio @profileArguments
        if ($LASTEXITCODE -ne 0) { throw "$codec GPU smoke failed. See $result" }

        & (Join-Path $PSScriptRoot 'Validate-Output.ps1') -Output $output -Result $result `
            -Codec $codec -Width 640 -Height 360 -Fps 60 -ExpectedFrames 120 -ExpectAudio
        if ($LASTEXITCODE -ne 0) { throw "$codec output validation failed. See $result" }
        $run = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        if ($run.input_recycling.requested -ne $recycleInput -or $run.input_recycling.invalid_events -ne 0) { throw 'Input-recycling status mismatch.' }
        if ($recycleInput -and ($run.input_recycling.input_releases -ne 120 -or $run.input_recycling.output_completions -ne 120)) { throw 'Incomplete input recycling.' }
        if ($run.output_wait.requested -ne $optimizeWait) { throw 'Output-wait option did not reach the native encoder.' }
        if (-not $optimizeWait -and $run.output_wait.reason -ne 'disabled') { throw 'Legacy output-wait mode changed.' }
        if ($profiling) {
            if ($run.profiling.version -ne 1 -or $run.profiling.accepted_frames -ne 120 -or $run.profiling.completed_frames -ne 120) {
                throw 'Profile frame counters do not match completed output.'
            }
            foreach ($stage in @('slot_wait', 'copy_resource_cpu', 'slot_residence', 'bitstream_mux')) {
                if ($run.profiling.stages.$stage.count -ne 120) { throw "Unexpected profile count: $stage" }
            }
            foreach ($stage in @('audio_write', 'writer_sample', 'finalize', 'mp4_finalize')) {
                if ($run.profiling.stages.$stage.count -lt 1) { throw "Missing profile stage: $stage" }
            }
            if (Test-Path -LiteralPath "$output.amf_log.txt") { throw 'Profile-only mode unexpectedly wrote a debug log.' }
        } elseif ($null -ne $run.profiling) { throw 'Unrequested profiling data was recorded.' }
        Write-Host "$caseName GPU smoke and correctness validation: passed"
        }
       }
      }
    }
    $managedRoot = Join-Path $runRoot ('managed-' + [Guid]::NewGuid().ToString('N'))
    & dotnet $testAssembly --gpu $managedRoot (Join-Path $repositoryRoot 'artifacts\bin\AmfNative.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Managed writer GPU integration checks failed.' }
    foreach ($codec in @('h264', 'hevc')) {
        foreach ($mode in @('off', 'profile')) {
          foreach ($inputMode in @('cpu-read', 'gpu-direct')) {
           foreach ($waitMode in @('wait-off', 'wait-on')) {
            foreach ($recycleMode in @('recycle-off', 'recycle-on')) {
             foreach ($mfNv12Mode in @('off', 'on')) {
            $suffix = if ($mfNv12Mode -eq 'on') { '-mf-nv12-on' } else { '' }
            $caseRoot = Join-Path $managedRoot "$codec-$mode-$inputMode-$waitMode-$recycleMode$suffix"
            & (Join-Path $PSScriptRoot 'Validate-Output.ps1') -Output (Join-Path $caseRoot 'output.mp4') `
                -Result (Join-Path $caseRoot 'run.json') -Codec $codec -Width 640 -Height 360 -Fps 60 -ExpectedFrames 120 -ExpectAudio
            if ($LASTEXITCODE -ne 0) { throw "Managed $codec-$mode-$inputMode-$mfNv12Mode output validation failed." }
             }
            }
           }
          }
        }
    }
    & $bench output-wait-stop-test --output (Join-Path $runRoot 'lifecycle')
    if ($LASTEXITCODE -ne 0) { throw 'Output-wait idle/active stop tests failed.' }
    & $bench output-wait-stop-test --recycle-input --output (Join-Path $runRoot 'recycling-lifecycle')
    if ($LASTEXITCODE -ne 0) { throw 'Input-recycling idle/active stop tests failed.' }
    & (Join-Path $repositoryRoot 'tests/MfAmfPathBench/Test-PipelineNative.ps1') `
        -RunDirectory (Join-Path $runRoot ('pipeline-' + [Guid]::NewGuid().ToString('N')))
}

# Expected negative CLI checks leave LASTEXITCODE non-zero even when every
# assertion above passed. Normalize it so CI observes the script result.
$global:LASTEXITCODE = 0
