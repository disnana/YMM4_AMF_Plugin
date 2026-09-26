#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$runPath = Join-Path $RunDirectory 'run.json'
$run = Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json
if ($run.status -ne 'writer_returned_validation_required') { throw "Writer did not complete: $($run.status)" }
$output = Join-Path $RunDirectory 'output.mp4'
$result = [ordered]@{ status = 'failed'; scope = 'Full decode, every frame marker, stream metadata and duration; not perceptual quality or lip-sync' }
try {
    $probeText = & ffprobe -v error -count_frames -show_streams -show_format -of json $output | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'ffprobe failed' }
    $probe = $probeText | ConvertFrom-Json
    $result.metadata = $probe
    $video = @($probe.streams | Where-Object codec_type -eq video)
    if ($video.Count -ne 1) { throw 'Expected one video stream' }
    $video = $video[0]
    $request = $run.requested
    if ($video.codec_name -ne 'h264' -or $video.width -ne $request.Width -or $video.height -ne $request.Height -or
        [int]$video.nb_read_frames -ne $request.Frames -or $video.r_frame_rate -ne "$($request.Fps)/1") { throw 'Video format or frame count mismatch' }
    & ffmpeg -v error -xerror -i $output -f null NUL
    if ($LASTEXITCODE -ne 0) { throw 'Full decode failed' }
    $result.decode = 'passed'
    $markerSize = [Math]::Max(4, [Math]::Floor([Math]::Min($request.Width, $request.Height) / 32))
    $markerFile = Join-Path $RunDirectory 'markers.gray'
    if (Test-Path -LiteralPath $markerFile) { throw 'Validation marker output already exists' }
    $filter = "crop=$($markerSize * 8):$($markerSize * 2):0:0,scale=8:2:flags=area,format=gray"
    & ffmpeg -v error -xerror -n -i $output -map 0:v:0 -vf $filter -f rawvideo $markerFile
    if ($LASTEXITCODE -ne 0) { throw 'Marker extraction failed' }
    $markers = [IO.File]::ReadAllBytes($markerFile)
    if ($markers.Length -ne $request.Frames * 16) { throw 'Marker count mismatch' }
    for ($frame = 0; $frame -lt $request.Frames; $frame++) {
        for ($bit = 0; $bit -lt 16; $bit++) {
            $sample = $markers[$frame * 16 + $bit]
            $one = ($frame -band (1 -shl $bit)) -ne 0
            if (($one -and $sample -lt 160) -or (-not $one -and $sample -gt 96)) { throw "Frame $frame marker bit $bit corrupt or out of order: $sample" }
        }
    }
    $result.frame_order = 'passed_all_frames'
    $result.color = 'metadata_recorded_only_not_pixel_equivalence'
    if ($request.Audio) {
        $audio = @($probe.streams | Where-Object codec_type -eq audio)
        if ($audio.Count -ne 1 -or $audio[0].codec_name -ne 'aac' -or $audio[0].sample_rate -ne '48000' -or $audio[0].channels -ne 2) { throw 'Audio stream mismatch' }
        $delta = [Math]::Abs(([double]$audio[0].duration - [double]$video.duration) * 1000)
        $result.audio_duration_delta_ms = $delta
        $result.audio_start_delta_ms = ([double]$audio[0].start_time - [double]$video.start_time) * 1000
        if ($delta -gt 50) { throw "Audio duration delta exceeds 50 ms: $delta" }
        $result.audio = 'passed_duration_only'
    }
    $result.status = 'passed'
} catch { $result.error = $_.Exception.Message }
$validationPath = Join-Path $RunDirectory 'validation.json'
$result | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $validationPath -Encoding utf8
if ($result.status -ne 'passed') { throw $result.error }
Write-Host "Validated $RunDirectory"
