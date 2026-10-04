$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repositoryRoot 'scripts/Mp4Timing.Common.ps1')
function U32([uint32]$Value) {
    $bytes = [BitConverter]::GetBytes($Value)
    [Array]::Reverse($bytes)
    return $bytes
}
function Box([string]$Type, [byte[]]$Payload) {
    return [byte[]]((U32 ($Payload.Length + 8)) + [Text.Encoding]::ASCII.GetBytes($Type) + $Payload)
}
function Fixture([uint32]$Rate, [uint32]$Samples, [uint32]$TrackDuration) {
    $movie = [byte[]]([byte[]]::new(12) + (U32 90000) + (U32 21600000))
    $track = [byte[]]([byte[]]::new(12) + (U32 2) + (U32 0) + (U32 $TrackDuration))
    $media = [byte[]]([byte[]]::new(12) + (U32 $Rate) + (U32 $Samples))
    $handler = [byte[]]([byte[]]::new(8) + [Text.Encoding]::ASCII.GetBytes('soun'))
    $mdia = Box 'mdia' ([byte[]]((Box 'mdhd' $media) + (Box 'hdlr' $handler)))
    $trak = Box 'trak' ([byte[]]((Box 'tkhd' $track) + $mdia))
    return Box 'moov' ([byte[]]((Box 'mvhd' $movie) + $trak))
}
$root = Join-Path $repositoryRoot ('artifacts/runs/mp4-timing-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$path = Join-Path $root 'fixture.mp4'
$checks = 0
foreach ($rate in @(32000, 44100, 48000)) {
    $samples = [uint32]($rate * 240)
    [IO.File]::WriteAllBytes($path, (Fixture $rate $samples 21600000))
    $report = Assert-Mp4AudioTrackTiming $path
    if ($report.status -ne 'passed') { throw 'Valid four-minute track rejected.' }
    $checks++
    # Reproduce the released writer: audio sample ticks were copied into tkhd.
    [IO.File]::WriteAllBytes($path, (Fixture $rate $samples $samples))
    $rejected = $false
    try { Assert-Mp4AudioTrackTiming $path | Out-Null }
    catch { if ($_.Exception.Message -notlike '*wrong clock*') { throw }; $rejected = $true }
    if (-not $rejected) { throw 'Released wrong-clock regression was accepted.' }
    $checks++
}
# AAC sample count need not map to an integer movie tick at 44.1 kHz.
[IO.File]::WriteAllBytes($path, (Fixture 44100 1024 2089))
Assert-Mp4AudioTrackTiming $path | Out-Null
$checks++
[IO.File]::WriteAllBytes($path, [byte[]](0, 0, 0, 16, 109, 111, 111, 118))
$rejected = $false
try { Assert-Mp4AudioTrackTiming $path | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'Truncated MP4 box was accepted.' }
$checks++
Write-Host "MP4 audio timing regression checks: $checks passed"
