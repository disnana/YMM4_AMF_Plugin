# CPU-only helpers shared by the comparison script and contract tests.
function Invoke-QualityMediaTool {
    param([string]$Executable, [string[]]$Arguments, [string]$Directory, [int]$TimeoutSeconds = 300)
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.WorkingDirectory = $Directory
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while (-not $process.WaitForExit(500)) {
            if ([DateTime]::UtcNow -gt $deadline) { throw "Media analysis timed out after $TimeoutSeconds seconds." }
        }
        $output = $stdout.GetAwaiter().GetResult()
        $errorText = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Media analysis failed ($($process.ExitCode)): $errorText" }
        return [pscustomobject]@{ Output = $output; ErrorText = $errorText }
    }
    finally {
        if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
        $process.Dispose()
    }
}

function ConvertFrom-QualityNumber([string]$Value) {
    if ($Value -eq 'inf') { return [double]::PositiveInfinity }
    $number = [double]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture)
    if (-not [double]::IsFinite($number)) { throw "Invalid metric: $Value" }
    return $number
}

function Get-QualityVideoContract($Probe, [string]$Label) {
    $streams = @($Probe.streams)
    if ($streams.Count -ne 1) { throw "$Label requires exactly one selected video stream." }
    $stream = $streams[0]
    # No implicit scaling, range conversion, chroma subsampling, or HDR tonemapping.
    $expected = @{ pix_fmt = 'yuv420p'; color_range = 'tv'; color_space = 'bt709';
        color_transfer = 'bt709'; color_primaries = 'bt709' }
    foreach ($key in $expected.Keys) {
        if ($stream.$key -ne $expected[$key]) { throw "$Label requires $key=$($expected[$key]); found '$($stream.$key)'. See docs/export-quality-comparison.md." }
    }
    if ($stream.width -le 0 -or $stream.height -le 0) { throw "$Label has invalid dimensions." }
    if ($stream.r_frame_rate -notmatch '^(\d+)/(\d+)$') { throw "$Label has no valid frame rate." }
    $numerator = [double]$Matches[1]; $denominator = [double]$Matches[2]
    if ($numerator -le 0 -or $denominator -le 0) { throw "$Label has no positive frame rate." }
    $fps = $numerator / $denominator
    $frames = @($Probe.frames)
    if ($frames.Count -eq 0) { throw "$Label has no decoded frames." }
    $first = $null
    for ($index = 0; $index -lt $frames.Count; $index++) {
        $value = [string]$frames[$index].best_effort_timestamp_time
        if (-not $value) { throw "$Label frame $index has no timestamp." }
        $timestamp = ConvertFrom-QualityNumber $value
        if (-not [double]::IsFinite($timestamp)) { throw "$Label has invalid timestamps." }
        if ($index -eq 0) { $first = $timestamp }
        # Matroska's millisecond timebase can round 60 fps timestamps.
        if ([Math]::Abs(($timestamp - $first) - $index / $fps) -gt 0.002) {
            throw "$Label frame $index is not on the declared CFR timeline (2 ms tolerance)."
        }
    }
    return [pscustomobject]@{
        width = [int]$stream.width; height = [int]$stream.height; fps = $fps; frames = $frames.Count
        codec = $stream.codec_name; pixel_format = $stream.pix_fmt
        color = 'bt709_limited_8bit_420'; duration_seconds = $frames.Count / $fps
    }
}

function Assert-QualityVideoMatch($Reference, $Candidate, [string]$Label) {
    if ($Reference.width -ne $Candidate.width -or $Reference.height -ne $Candidate.height) {
        throw "$Label resolution does not match the reference. Automatic scaling is not allowed."
    }
    if ($Reference.frames -ne $Candidate.frames) { throw "$Label decoded frame count does not match the reference." }
    if ([Math]::Abs($Reference.fps - $Candidate.fps) -gt 0.000001) { throw "$Label frame rate does not match the reference." }
}

function Get-QualityMetricReport([string[]]$PsnrLines, [string[]]$SsimLines, [string]$Log, [int]$Frames) {
    $psnr = @($PsnrLines | Where-Object { $_ -match '^n:\d+ ' })
    $ssim = @($SsimLines | Where-Object { $_ -match '^n:\d+ ' })
    if ($psnr.Count -ne $Frames -or $ssim.Count -ne $Frames) {
        throw "Metric frame count mismatch: expected $Frames, PSNR $($psnr.Count), SSIM $($ssim.Count)."
    }
    $scores = [Collections.Generic.List[double]]::new()
    for ($index = 0; $index -lt $Frames; $index++) {
        foreach ($line in @($psnr[$index], $ssim[$index])) {
            if ($line -notmatch '^n:(\d+) ' -or [int]$Matches[1] -ne $index + 1) { throw 'Metric frame order mismatch.' }
        }
        if ($ssim[$index] -notmatch '\bAll:([-0-9.]+)') { throw 'Missing per-frame SSIM score.' }
        $scores.Add((ConvertFrom-QualityNumber $Matches[1]))
    }
    # FFmpeg's aggregate PSNR uses mean MSE, not the arithmetic mean of frame dB values.
    if ($Log -notmatch 'PSNR[^\r\n]*average:(inf|[0-9.]+)') { throw 'Missing aggregate PSNR.' }
    $averagePsnr = ConvertFrom-QualityNumber $Matches[1]
    $sorted = @($scores | Sort-Object)
    return [pscustomobject]@{
        frames = $Frames
        psnr_average_db = if ([double]::IsPositiveInfinity($averagePsnr)) { 'inf' } else { $averagePsnr }
        ssim_mean = ($scores | Measure-Object -Average).Average
        ssim_min = $sorted[0]
        ssim_p05 = $sorted[[Math]::Max(0, [int][Math]::Ceiling($Frames * 0.05) - 1)]
        ssim_p50 = $sorted[[Math]::Max(0, [int][Math]::Ceiling($Frames * 0.50) - 1)]
    }
}
