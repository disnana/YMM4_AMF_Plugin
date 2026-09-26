[CmdletBinding()]
param([switch]$Integration)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repositoryRoot 'scripts/ExportQuality.Common.ps1')
$script:qualityChecks = 0
function Assert-QualityTest([bool]$Condition, [string]$Message) {
    $script:qualityChecks++
    if (-not $Condition) { throw $Message }
}
function Assert-QualityThrows([scriptblock]$Action, [string]$Pattern) {
    $caught = $null
    try { & $Action | Out-Null } catch { $caught = $_.Exception.Message }
    Assert-QualityTest ($null -ne $caught -and $caught -like "*$Pattern*") "Expected '$Pattern', found '$caught'."
}
function New-QualityProbe {
    return [pscustomobject]@{
        streams = @([pscustomobject]@{ width = 96; height = 64; codec_name = 'h264'; pix_fmt = 'yuv420p';
            r_frame_rate = '30/1'; color_range = 'tv'; color_space = 'bt709'; color_transfer = 'bt709'; color_primaries = 'bt709' })
        frames = @([pscustomobject]@{ best_effort_timestamp_time = '2.000000' },
            [pscustomobject]@{ best_effort_timestamp_time = '2.033333' })
    }
}
$reference = Get-QualityVideoContract (New-QualityProbe) 'reference'
Assert-QualityTest ($reference.frames -eq 2 -and $reference.fps -eq 30) 'Valid CFR stream rejected.'
foreach ($field in @('pix_fmt', 'color_range', 'color_space', 'color_transfer', 'color_primaries')) {
    $probe = New-QualityProbe
    $probe.streams[0].$field = 'unknown'
    Assert-QualityThrows { Get-QualityVideoContract $probe 'invalid' } 'requires'
}
$probe = New-QualityProbe
$probe.frames[1].best_effort_timestamp_time = '2.060000'
Assert-QualityThrows { Get-QualityVideoContract $probe 'vfr' } 'CFR timeline'
$probe.frames[1].best_effort_timestamp_time = 'N/A'
Assert-QualityThrows { Get-QualityVideoContract $probe 'invalid' } 'correct format'
$probe = New-QualityProbe
$probe.streams[0].r_frame_rate = '0/0'
Assert-QualityThrows { Get-QualityVideoContract $probe 'invalid' } 'positive frame rate'
foreach ($field in @('width', 'fps', 'frames')) {
    $candidate = $reference | Select-Object *
    $candidate.$field++
    Assert-QualityThrows { Assert-QualityVideoMatch $reference $candidate 'candidate' } 'match'
}
$psnr = @('n:1 mse_avg:0.00 psnr_avg:inf', 'n:2 mse_avg:0.00 psnr_avg:inf')
$ssim = @('n:1 All:1.000000 (inf)', 'n:2 All:1.000000 (inf)')
$metric = Get-QualityMetricReport $psnr $ssim 'PSNR y:inf u:inf v:inf average:inf min:inf max:inf' 2
Assert-QualityTest ($metric.psnr_average_db -eq 'inf' -and $metric.ssim_p05 -eq 1) 'Infinite PSNR must serialize safely.'
$ssim = @('n:1 All:0.900000', 'n:2 All:1.000000')
$metric = Get-QualityMetricReport $psnr $ssim 'PSNR y:30.0 average:31.5 min:30.0 max:32.0' 2
Assert-QualityTest ($metric.psnr_average_db -eq 31.5 -and $metric.ssim_mean -eq 0.95 -and $metric.ssim_p05 -eq 0.9) 'Metric aggregation mismatch.'
Assert-QualityThrows { Get-QualityMetricReport $psnr $ssim 'PSNR average:nan' 2 } 'Missing aggregate PSNR'
Assert-QualityThrows { Get-QualityMetricReport $psnr $ssim 'PSNR average:31.5' 3 } 'frame count'
Assert-QualityThrows { Get-QualityMetricReport $psnr @('n:2 All:0.9', 'n:1 All:1.0') 'PSNR average:31.5' 2 } 'frame order'
$previousCulture = [Globalization.CultureInfo]::CurrentCulture
try {
    [Globalization.CultureInfo]::CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('de-DE')
    Assert-QualityTest ((ConvertFrom-QualityNumber '31.5') -eq 31.5) 'Metrics must use invariant decimals.'
}
finally { [Globalization.CultureInfo]::CurrentCulture = $previousCulture }

if ($Integration) {
    $ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
    $fixtureRoot = Join-Path $repositoryRoot ('artifacts/runs/quality-tests-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    $referencePath = Join-Path $fixtureRoot 'reference.mp4'
    $lossyPath = Join-Path $fixtureRoot 'lossy.mp4'
    $shortPath = Join-Path $fixtureRoot 'short.mp4'
    # Define the generated fixture's interpretation on its AVFrames as well as
    # the codec context; no user input is retagged by the comparison tool.
    $colorArguments = @('-vf', 'setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709',
        '-color_range', 'tv', '-colorspace', 'bt709', '-color_trc', 'bt709', '-color_primaries', 'bt709')
    Invoke-QualityMediaTool $ffmpeg (@('-hide_banner', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i',
        'testsrc2=size=96x64:rate=30', '-frames:v', '12', '-c:v', 'libx264', '-qp', '0', '-pix_fmt', 'yuv420p') + $colorArguments + $referencePath) $fixtureRoot | Out-Null
    Invoke-QualityMediaTool $ffmpeg (@('-hide_banner', '-nostdin', '-v', 'error', '-i', $referencePath,
        '-c:v', 'libx264', '-crf', '35', '-pix_fmt', 'yuv420p') + $colorArguments + $lossyPath) $fixtureRoot | Out-Null
    Invoke-QualityMediaTool $ffmpeg (@('-hide_banner', '-nostdin', '-v', 'error', '-i', $referencePath,
        '-frames:v', '11', '-c:v', 'libx264', '-crf', '35', '-pix_fmt', 'yuv420p') + $colorArguments + $shortPath) $fixtureRoot | Out-Null
    $compare = Join-Path $repositoryRoot 'scripts/Compare-ExportQuality.ps1'
    # Labels test report plumbing; these CPU fixtures are NOT actual MF/AMF exports.
    $better = & $compare -Reference $referencePath -MfOutput $lossyPath -AmfOutput $referencePath
    Assert-QualityTest ($better.Summary.metrics.amf.psnr_average_db -eq 'inf') 'Identical reference must have infinite PSNR.'
    Assert-QualityTest ($better.Summary.indicators.amf_ssim_mean_not_lower -and $better.Summary.indicators.amf_psnr_not_lower) 'Better candidate not detected.'
    Assert-QualityTest ($better.Summary.metrics.mf.frames -eq 12 -and $better.Summary.metrics.amf.frames -eq 12) 'All frames must be measured.'
    $worse = & $compare -Reference $referencePath -MfOutput $referencePath -AmfOutput $lossyPath
    Assert-QualityTest (-not $worse.Summary.indicators.amf_psnr_not_lower -and -not $worse.Summary.indicators.amf_ssim_mean_not_lower) 'Quality regression not detected.'
    Assert-QualityTest ($worse.Summary.perceptual_quality -like 'not_determined*') 'Objective metrics must not claim perceptual superiority.'
    Assert-QualityThrows { & $compare -Reference $referencePath -MfOutput $lossyPath -AmfOutput $shortPath } 'frame count'
    Write-Host "Quality integration fixtures: $fixtureRoot"
}
Write-Host "Export-quality checks: $script:qualityChecks passed (integration=$($Integration.IsPresent))"
$global:LASTEXITCODE = 0
