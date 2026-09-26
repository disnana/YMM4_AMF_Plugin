#requires -Version 7.0
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
function Median($values) {
    $sorted = @($values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    ($sorted[[int][Math]::Floor(($sorted.Count - 1) / 2)] + $sorted[[int][Math]::Floor($sorted.Count / 2)]) / 2
}
$rows = @(foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -Filter run.json -File) {
    $run = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    if ($run.status -ne 'writer_returned_validation_required') { throw "Failed run: $($file.FullName)" }
    $validation = Get-Content -LiteralPath (Join-Path $file.DirectoryName 'validation.json') -Raw | ConvertFrom-Json
    if ($validation.status -ne 'passed') { throw "Unvalidated run: $($file.FullName)" }
    $r = $run.requested
    $mode = if ($r.RecycleInput) { if ($r.OutputWait) { 'both' } else { 'recycle' } } elseif ($r.OutputWait) { 'wait' } else { 'legacy' }
    $profileFile = Join-Path $file.DirectoryName 'output.mp4.amf_profile.json'
    $p = if (Test-Path -LiteralPath $profileFile) { Get-Content -LiteralPath $profileFile -Raw | ConvertFrom-Json } else { $null }
    [pscustomobject]@{
        path = [IO.Path]::GetRelativePath($root, $file.DirectoryName)
        group = "$($r.AmfQuality)-$($r.Width)x$($r.Height)-frames$($r.Frames)-pool$($r.Pool)-$mode-profile$($r.Profile)"
        quality = $r.AmfQuality; pool = $r.Pool; mode = $mode; width = $r.Width; height = $r.Height; frames = $r.Frames; profile = $r.Profile
        export_ms = $run.export_ms; fps = $run.completed_fps; create_plus_export_ms = $run.writer_create_plus_export_ms
        video_p50_ms = $run.stages.write_video.p50_ms; video_p95_ms = $run.stages.write_video.p95_ms
        video_p99_ms = $run.stages.write_video.p99_ms; video_max_ms = $run.stages.write_video.max_ms
        finalize_ms = $run.finalize_ms; audio_duration_delta_ms = $validation.audio_duration_delta_ms
        native_sha256 = $run.amf_native_sha256; managed_sha256 = $run.amf_managed_sha256; harness_sha256 = $run.bench_sha256
        texture_pool = $p.texture_pool; input_recycling = $p.input_recycling; native = $p.native; gpu_memory = $run.gpu_memory
    }
})
$groups = @($rows | Group-Object group | ForEach-Object {
    $g = $_.Group; $first = $g[0]
    [pscustomobject]@{
        group = $_.Name; quality = $first.quality; pool = $first.pool; mode = $first.mode; width = $first.width; height = $first.height
        frames = $first.frames; profile = $first.profile; count = $g.Count
        median_export_ms = Median $g.export_ms; min_export_ms = ($g.export_ms | Measure-Object -Minimum).Minimum
        max_export_ms = ($g.export_ms | Measure-Object -Maximum).Maximum; median_fps = Median $g.fps
        median_create_plus_export_ms = Median $g.create_plus_export_ms; median_finalize_ms = Median $g.finalize_ms
        median_run_video_p50_ms = Median $g.video_p50_ms; median_run_video_p95_ms = Median $g.video_p95_ms
        median_run_video_p99_ms = Median $g.video_p99_ms; worst_video_max_ms = ($g.video_max_ms | Measure-Object -Maximum).Maximum
    }
})
$report = [ordered]@{
    status = 'validated'; scope = 'Synthetic actual AMF writer DLL, not YMM4 project E2E. Timing/Profile runs are separate. No discarded outliers.'
    runs = $rows.Count; frames = ($rows.frames | Measure-Object -Sum).Sum; groups = $groups; cases = $rows
}
$destination = Join-Path $root 'summary.json'
if (Test-Path -LiteralPath $destination) { throw 'Summary already exists' }
$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $destination -Encoding utf8
$groups | Where-Object { -not $_.profile -and $_.frames -eq 900 } |
    Format-Table quality,pool,mode,count,median_export_ms,median_fps,min_export_ms,max_export_ms
