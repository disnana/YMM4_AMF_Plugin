#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../scripts/ExportQuality.Common.ps1')
$directory = (Resolve-Path -LiteralPath $RunDirectory).ProviderPath
$r = Get-Content -LiteralPath (Join-Path $directory 'run.json') -Raw | ConvertFrom-Json
if ($r.requested.Mode -ne 'reference' -or $r.status -ne 'writer_returned_validation_required') { throw 'Not a completed reference capture' }
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$capture = Join-Path $directory 'reference-bgra.mkv'
$decoded = Invoke-QualityMediaTool $ffmpeg @('-nostdin','-v','error','-i',$capture,'-map','0:v:0','-an',
    '-pix_fmt','bgra','-f','hash','-hash','sha256','-') $directory 120
if ($decoded.Output -notmatch 'SHA256=([0-9a-fA-F]+)' -or $Matches[1] -ne $r.reference_bgra_sha256) { throw 'Lossless reference did not reproduce the exact captured BGRA bytes' }
# Explicit analysis convention: BT.709 matrix, full-range nonlinear RGB values to
# limited 8-bit 4:2:0, bilinear chroma resampling. No transfer-function remapping.
# This is not a claim that GPU converters produce identical chroma rounding.
$filter = 'scale=in_range=pc:out_range=tv:out_color_matrix=bt709:flags=bilinear,format=yuv420p,setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709'
$destination = Join-Path $directory 'reference-420-tagged.mkv'
# Set frame properties as well as encoder options: this FFmpeg build otherwise
# retains unknown input transfer/primary values. Sample-exact QP0 storage is checked below.
$conversion = Invoke-QualityMediaTool $ffmpeg @('-nostdin','-v','error','-n','-i',$capture,'-an','-vf',$filter,
    '-c:v','libx264','-qp','0','-preset','ultrafast','-threads','4','-color_range','tv','-colorspace','bt709',
    '-color_primaries','bt709','-color_trc','bt709',$destination) $directory 120
$normalized = Invoke-QualityMediaTool $ffmpeg @('-nostdin','-v','error','-i',$capture,'-an','-vf',$filter,
    '-pix_fmt','yuv420p','-f','hash','-hash','sha256','-') $directory 120
$lossless = Invoke-QualityMediaTool $ffmpeg @('-nostdin','-v','error','-i',$destination,'-an',
    '-pix_fmt','yuv420p','-f','hash','-hash','sha256','-') $directory 120
if ($normalized.Output.Trim() -ne $lossless.Output.Trim()) { throw 'Canonical 4:2:0 reference is not sample-exact' }
[ordered]@{ exact_bgra_round_trip='passed_sha256'; raw_bgra_sha256=$r.reference_bgra_sha256;
    reference_bgra_sha256=(Get-FileHash -LiteralPath $capture).Hash;
    reference_420_sha256=(Get-FileHash -LiteralPath $destination).Hash; filter=$filter;
    exact_canonical_420_round_trip='passed_sha256'; raw_420_hash=$normalized.Output.Trim();
    caveat='PSNR/SSIM include encoder and color-conversion differences. 4:2:0 conversion is lossy relative to captured BGRA; BGRA FFV1 and canonical QP0 H.264 storage are lossless.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'reference-quality-validation.json') -Encoding utf8
Write-Host "Reference verified and normalized: $destination"
