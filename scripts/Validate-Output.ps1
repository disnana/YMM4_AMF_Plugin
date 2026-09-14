[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Output,

    [Parameter(Mandatory)]
    [string]$Result,

    [Parameter(Mandatory)]
    [ValidateSet('h264', 'hevc')]
    [string]$Codec,

    [Parameter(Mandatory)]
    [ValidateRange(2, 16384)]
    [int]$Width,

    [Parameter(Mandatory)]
    [ValidateRange(2, 16384)]
    [int]$Height,

    [Parameter(Mandatory)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$Fps,

    [Parameter(Mandatory)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$ExpectedFrames,

    [switch]$ExpectAudio
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param(
        [Parameter(Mandatory)]
        [bool]$Condition,

        [Parameter(Mandatory)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-NormalizedPath {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathFullyQualified($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

$outputPath = Get-NormalizedPath $Output
$resultPath = Get-NormalizedPath $Result
Assert-Condition (Test-Path -LiteralPath $outputPath -PathType Leaf) "Output file was not found: $outputPath"
Assert-Condition (Test-Path -LiteralPath $resultPath -PathType Leaf) "Result file was not found: $resultPath"

$ffprobe = Get-Command ffprobe -ErrorAction SilentlyContinue
$ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
Assert-Condition ($null -ne $ffprobe -and $null -ne $ffmpeg) 'Output validation requires ffmpeg and ffprobe on PATH.'

$probeText = & $ffprobe.Source -v error -count_frames `
    -show_entries 'stream=index,codec_name,codec_type,nb_read_frames,width,height,r_frame_rate,duration,color_range,color_space,color_transfer,color_primaries,sample_rate,channels' `
    -of json $outputPath | Out-String
Assert-Condition ($LASTEXITCODE -eq 0) 'ffprobe failed.'
$probe = $probeText | ConvertFrom-Json
$video = @($probe.streams | Where-Object { $_.codec_type -eq 'video' })
Assert-Condition ($video.Count -eq 1) "Expected exactly one video stream, found $($video.Count)."
$video = $video[0]
Assert-Condition ($video.codec_name -eq $Codec) "Expected $Codec video, found $($video.codec_name)."
Assert-Condition ([int]$video.width -eq $Width -and [int]$video.height -eq $Height) `
    "Expected ${Width}x${Height}, found $($video.width)x$($video.height)."
Assert-Condition ([int]$video.nb_read_frames -eq $ExpectedFrames) `
    "Expected $ExpectedFrames decoded frames, found $($video.nb_read_frames)."
Assert-Condition ($video.r_frame_rate -eq "$Fps/1") "Expected $Fps/1 frame rate, found $($video.r_frame_rate)."

& $ffmpeg.Source -v error -i $outputPath -f null NUL
Assert-Condition ($LASTEXITCODE -eq 0) 'Full output decode failed.'

# RadeonBench writes the low 16 bits of the frame index into an 8x2 grid in
# the top-left corner. Downsampling each solid marker cell to one gray byte
# keeps the validation file at only 16 bytes per frame.
$markerSize = [Math]::Max(4, [Math]::Floor([Math]::Min($Width, $Height) / 32))
$cropWidth = 8 * $markerSize
$cropHeight = 2 * $markerSize
$markerPath = Join-Path ([System.IO.Path]::GetTempPath()) "ymm4-amf-markers-$([guid]::NewGuid().ToString('N')).gray"
try {
    $filter = "crop=${cropWidth}:${cropHeight}:0:0,scale=8:2:flags=area,format=gray"
    & $ffmpeg.Source -v error -y -i $outputPath -map '0:v:0' -vf $filter -f rawvideo $markerPath
    Assert-Condition ($LASTEXITCODE -eq 0) 'Frame-marker decode failed.'
    $markers = [System.IO.File]::ReadAllBytes($markerPath)
}
finally {
    if (Test-Path -LiteralPath $markerPath) {
        Remove-Item -LiteralPath $markerPath -Force
    }
}

$bytesPerMarker = 16
Assert-Condition ($markers.Length -eq $ExpectedFrames * $bytesPerMarker) `
    "Expected $($ExpectedFrames * $bytesPerMarker) marker bytes, found $($markers.Length)."
for ($frame = 0; $frame -lt $ExpectedFrames; ++$frame) {
    $decodedIndex = 0
    $expectedIndex = $frame -band 0xffff
    for ($bit = 0; $bit -lt 16; ++$bit) {
        $sample = $markers[$frame * $bytesPerMarker + $bit]
        $expectedOne = ($expectedIndex -band (1 -shl $bit)) -ne 0
        if ($expectedOne) {
            Assert-Condition ($sample -ge 160) "Frame $frame marker bit $bit is too dark ($sample)."
            $decodedIndex = $decodedIndex -bor (1 -shl $bit)
        }
        else {
            Assert-Condition ($sample -le 96) "Frame $frame marker bit $bit is too bright ($sample)."
        }
    }
    Assert-Condition ($decodedIndex -eq $expectedIndex) `
        "Frame-order validation failed at decoded frame $frame (marker=$decodedIndex)."
}

$expectedColor = @{
    color_range = 'tv'
    color_space = 'bt709'
    color_transfer = 'bt709'
    color_primaries = 'bt709'
}
foreach ($property in $expectedColor.Keys) {
    Assert-Condition ($video.$property -eq $expectedColor[$property]) `
        "Expected $property=$($expectedColor[$property]), found $($video.$property)."
}

$audioDurationDeltaMs = $null
if ($ExpectAudio) {
    $audio = @($probe.streams | Where-Object { $_.codec_type -eq 'audio' })
    Assert-Condition ($audio.Count -eq 1) "Expected exactly one audio stream, found $($audio.Count)."
    $audio = $audio[0]
    Assert-Condition ($audio.codec_name -eq 'aac') "Expected AAC audio, found $($audio.codec_name)."
    Assert-Condition ([int]$audio.sample_rate -eq 48000) "Expected 48000 Hz audio, found $($audio.sample_rate)."
    Assert-Condition ([int]$audio.channels -eq 2) "Expected stereo audio, found $($audio.channels) channels."
    $audioDurationDeltaMs = [Math]::Abs(([double]$audio.duration - [double]$video.duration) * 1000.0)
    Assert-Condition ($audioDurationDeltaMs -le 50.0) `
        "Audio/video duration delta is $audioDurationDeltaMs ms (limit: 50 ms)."
}

$run = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
Assert-Condition ($run.status -eq 'passed') "Encoder result status is $($run.status)."
$run.validation.decode = 'passed'
$run.validation.frame_order = 'passed'
$run.validation.color = 'passed_bt709_limited_metadata'
$run.validation.audio_sync = if ($ExpectAudio) { 'passed_duration_only' } else { 'not_requested' }
if ($ExpectAudio) {
    $run.validation | Add-Member -NotePropertyName audio_duration_delta_ms `
        -NotePropertyValue ([Math]::Round($audioDurationDeltaMs, 3)) -Force
}
$run | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $resultPath -Encoding utf8

Write-Host "Output validation passed: codec=$Codec frames=$ExpectedFrames frame_order=passed color=bt709/tv audio=$($ExpectAudio.IsPresent)"
