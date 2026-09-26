[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Reference,
    [Parameter(Mandatory)][string]$MfOutput,
    [Parameter(Mandatory)][string]$AmfOutput,
    [ValidateRange(10, 3600)][int]$TimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ExportQuality.Common.ps1')
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$ffprobe = (Get-Command ffprobe -ErrorAction Stop).Source
$files = [ordered]@{}
foreach ($item in @(@('reference', $Reference), @('mf', $MfOutput), @('amf', $AmfOutput))) {
    $path = (Resolve-Path -LiteralPath $item[1]).ProviderPath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Not a file: $path" }
    $files[$item[0]] = $path
}
$runRoot = Join-Path (Split-Path -Parent $PSScriptRoot) ('artifacts/runs/export-quality-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$inputs = [ordered]@{}
foreach ($name in $files.Keys) {
    Write-Host "Inspecting $name frames..."
    $probe = Invoke-QualityMediaTool $ffprobe @('-v', 'error', '-protocol_whitelist', 'file,pipe',
        '-select_streams', 'v:0', '-show_streams', '-show_frames', '-show_entries',
        'stream=codec_name,width,height,pix_fmt,r_frame_rate,color_range,color_space,color_transfer,color_primaries:frame=best_effort_timestamp_time',
        '-of', 'json', $files[$name]) $runRoot $TimeoutSeconds
    $probe.Output | Set-Content -LiteralPath (Join-Path $runRoot "$name.probe.json") -Encoding utf8
    $contract = Get-QualityVideoContract ($probe.Output | ConvertFrom-Json) $name
    $inputs[$name] = [ordered]@{
        path = $files[$name]; sha256 = (Get-FileHash -LiteralPath $files[$name]).Hash
        file_bytes = (Get-Item -LiteralPath $files[$name]).Length; video = $contract
    }
}
Assert-QualityVideoMatch $inputs.reference.video $inputs.mf.video 'MF'
Assert-QualityVideoMatch $inputs.reference.video $inputs.amf.video 'AMF'
$metrics = [ordered]@{}
foreach ($name in @('mf', 'amf')) {
    Write-Host "Measuring $name against the common reference..."
    # After validating CFR timestamps/counts, pair by frame index. Do not repeat
    # the last reference frame or silently compare only the shorter input.
    $graph = "[0:v:0]settb=1/1,setpts=N,split=2[c0][c1];[1:v:0]settb=1/1,setpts=N,split=2[r0][r1];" +
        "[c0][r0]psnr=stats_file=$name.psnr.log`:shortest=1`:repeatlast=0[p];" +
        "[c1][r1]ssim=stats_file=$name.ssim.log`:shortest=1`:repeatlast=0[s]"
    $result = Invoke-QualityMediaTool $ffmpeg @('-hide_banner', '-nostdin', '-v', 'info',
        '-protocol_whitelist', 'file,pipe', '-i', $files[$name], '-protocol_whitelist', 'file,pipe', '-i', $files.reference,
        '-filter_complex_threads', '1', '-filter_complex', $graph, '-map', '[p]', '-map', '[s]',
        '-an', '-fps_mode', 'passthrough', '-f', 'null', '-') $runRoot $TimeoutSeconds
    $result.ErrorText | Set-Content -LiteralPath (Join-Path $runRoot "$name.ffmpeg.log") -Encoding utf8
    $metrics[$name] = Get-QualityMetricReport (Get-Content -LiteralPath (Join-Path $runRoot "$name.psnr.log")) `
        (Get-Content -LiteralPath (Join-Path $runRoot "$name.ssim.log")) $result.ErrorText $inputs.reference.video.frames
}
$summary = [ordered]@{
    schema_version = 1; created = (Get-Date).ToString('o'); inputs = $inputs; metrics = $metrics
    indicators = [ordered]@{
        amf_psnr_not_lower = (ConvertFrom-QualityNumber $metrics.amf.psnr_average_db) -ge (ConvertFrom-QualityNumber $metrics.mf.psnr_average_db)
        amf_ssim_mean_not_lower = $metrics.amf.ssim_mean -ge $metrics.mf.ssim_mean
        amf_ssim_p05_not_lower = $metrics.amf.ssim_p05 -ge $metrics.mf.ssim_p05
        amf_to_mf_file_size_ratio = $inputs.amf.file_bytes / $inputs.mf.file_bytes
    }
    perceptual_quality = 'not_determined_requires_visual_review'
    export_speed = 'not_measured_by_quality_analysis'
    notes = @('Caller must supply the same uncompressed/lossless rendered frames as the common reference, not the MF output.',
        'All files must cover identical content and frame order; timestamps/counts cannot prove semantic alignment.',
        'SDR BT.709 limited yuv420p only. No automatic resize, color conversion, trimming, or frame duplication.',
        'PSNR/SSIM are reference fidelity indicators, not a universal perceptual-quality guarantee.',
        'Audio quality and audible A/V sync are not measured here. File size includes audio/container overhead.',
        'Use separate warmed-up AB/BA export timings. Analysis duration is not encoder performance.')
}
$version = Invoke-QualityMediaTool $ffmpeg @('-version') $runRoot $TimeoutSeconds
$version.Output | Set-Content -LiteralPath (Join-Path $runRoot 'ffmpeg-version.txt') -Encoding utf8
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $runRoot 'summary.json') -Encoding utf8
Write-Host "Quality report: $runRoot"
[pscustomobject]@{ ReportPath = (Join-Path $runRoot 'summary.json'); Summary = $summary }
