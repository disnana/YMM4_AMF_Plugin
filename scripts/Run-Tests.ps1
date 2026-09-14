[CmdletBinding()]
param(
    [ValidateSet('Unit', 'GpuSmoke', 'All')]
    [string]$Suite = 'Unit'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'Build.ps1') -Configuration Release
$bench = Join-Path $repositoryRoot 'artifacts\bin\RadeonBench.exe'

if ($Suite -in @('Unit', 'All')) {
    $parseFailures = @()
    foreach ($script in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File) {
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
}

if ($Suite -in @('GpuSmoke', 'All')) {
    $runRoot = Join-Path $repositoryRoot 'artifacts\runs\gpu-smoke'
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
    $ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if (-not $ffprobe -or -not $ffmpeg) {
        throw 'GpuSmoke requires ffmpeg and ffprobe on PATH so encoded output is not accepted without validation.'
    }
    foreach ($codec in @('h264', 'hevc')) {
        $caseRoot = Join-Path $runRoot $codec
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        $output = Join-Path $caseRoot 'output.mp4'
        $result = Join-Path $caseRoot 'run.json'
        & $bench run --output $output --result $result --codec $codec --width 640 --height 360 --fps 60 --frames 120 --bitrate-kbps 4000 --pool-size 4 --audio
        if ($LASTEXITCODE -ne 0) { throw "$codec GPU smoke failed. See $result" }

        & (Join-Path $PSScriptRoot 'Validate-Output.ps1') -Output $output -Result $result `
            -Codec $codec -Width 640 -Height 360 -Fps 60 -ExpectedFrames 120 -ExpectAudio
        if ($LASTEXITCODE -ne 0) { throw "$codec output validation failed. See $result" }
        Write-Host "$codec GPU smoke and correctness validation: passed"
    }
}

# Expected negative CLI checks leave LASTEXITCODE non-zero even when every
# assertion above passed. Normalize it so CI observes the script result.
$global:LASTEXITCODE = 0
