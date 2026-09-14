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
    & $bench 2>$null
    if ($LASTEXITCODE -eq 0) { throw 'CLI unexpectedly accepted missing arguments.' }
    & $bench run --pool-size 3 2>$null
    if ($LASTEXITCODE -eq 0) { throw 'CLI unexpectedly accepted an invalid pool size.' }
    Write-Host 'Unit/contract checks: passed'
}

if ($Suite -in @('GpuSmoke', 'All')) {
    $runRoot = Join-Path $repositoryRoot 'artifacts\runs\gpu-smoke'
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
    foreach ($codec in @('h264', 'hevc')) {
        $caseRoot = Join-Path $runRoot $codec
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        $output = Join-Path $caseRoot 'output.mp4'
        $result = Join-Path $caseRoot 'run.json'
        & $bench run --output $output --result $result --codec $codec --width 640 --height 360 --fps 60 --frames 120 --bitrate-kbps 4000 --pool-size 4 --audio
        if ($LASTEXITCODE -ne 0) { throw "$codec GPU smoke failed. See $result" }

        $ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
        $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
        if ($ffprobe -and $ffmpeg) {
            $probeJson = & $ffprobe.Source -v error -count_frames -show_entries 'stream=index,codec_name,nb_read_frames' -of json $output | ConvertFrom-Json
            $video = $probeJson.streams | Where-Object { $_.codec_name -eq $codec } | Select-Object -First 1
            if (-not $video -or [int]$video.nb_read_frames -ne 120) { throw "$codec frame-count validation failed." }
            & $ffmpeg.Source -v error -i $output -f null NUL
            if ($LASTEXITCODE -ne 0) { throw "$codec full-decode validation failed." }
            $runData = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
            $runData.validation.decode = 'passed'
            $runData | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $result -Encoding utf8
            Write-Host "$codec GPU smoke and full decode: passed"
        } else {
            Write-Warning "$codec encode passed; ffmpeg/ffprobe validation was not run because the tools are missing."
        }
    }
}

# Expected negative CLI checks leave LASTEXITCODE non-zero even when every
# assertion above passed. Normalize it so CI observes the script result.
$global:LASTEXITCODE = 0
