#requires -Version 7.0
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
function Median($values) {
    $sorted = @($values | Sort-Object)
    if (-not $sorted.Count) { return $null }
    ($sorted[[int][Math]::Floor(($sorted.Count - 1)/2)] + $sorted[[int][Math]::Floor($sorted.Count/2)]) / 2
}
$rows = @(foreach ($file in Get-ChildItem -LiteralPath $root -Filter run.json -File -Recurse) {
    $run = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    if ($run.status -ne 'writer_returned_validation_required') { throw "Failed run: $file" }
    $v = Get-Content -LiteralPath (Join-Path $file.DirectoryName 'validation.json') -Raw | ConvertFrom-Json
    if ($v.status -ne 'passed') { throw "Unvalidated: $file" }
    $r = $run.requested
    $mode = if ($r.AsyncSubmission) { if ($r.DedicatedDevice) {'both'} else {'worker'} } elseif ($r.DedicatedDevice) {'device'} else {'base'}
    $profile = Join-Path $file.DirectoryName 'output.mp4.amf_profile.json'
    $p = if (Test-Path -LiteralPath $profile) { Get-Content -LiteralPath $profile -Raw | ConvertFrom-Json } else { $null }
    if ($p) {
        if (-not $p.output_finalized -or $p.status -ne 'writer_disposed' -or -not $p.pipeline.initialized -or
            $p.pipeline.async_submission -ne $r.AsyncSubmission -or $p.pipeline.dedicated_device -ne $r.DedicatedDevice -or
            $p.pipeline.submitted_frames -ne $r.Frames -or $p.pipeline.queue_depth -ne 0 -or
            $p.native.accepted_frames -ne $r.Frames -or $p.native.completed_frames -ne $r.Frames -or
            $p.input_recycling.invalid_events -ne 0) { throw "Pipeline lifecycle mismatch: $profile" }
    }
    [pscustomobject]@{
        path=[IO.Path]::GetRelativePath($root,$file.DirectoryName)
        group="$($r.Mode)-$($r.AmfQuality)-$($r.Width)x$($r.Height)-f$($r.Frames)-pool$($r.Pool)-r$($r.RenderRepeats)-$mode-profile$($r.Profile)-recycle$($r.RecycleInput)-wait$($r.OutputWait)"
        quality=$r.AmfQuality; pool=$r.Pool; render_repeats=$r.RenderRepeats; mode=$mode; profile=$r.Profile; frames=$r.Frames
        fps=$run.completed_fps; export_ms=$run.export_ms; finalize_ms=$run.finalize_ms
        create_plus_export_ms=$run.writer_create_plus_export_ms; cpu=$run.process_cpu_ms; gc=$run.gc_collections
        p50=$run.stages.write_video.p50_ms; p95=$run.stages.write_video.p95_ms; p99=$run.stages.write_video.p99_ms; max=$run.stages.write_video.max_ms
        render=$run.stages.render_cpu; pipeline=$p.pipeline; native=$p.native; texture_pool=$p.texture_pool
        recycling=$p.input_recycling; gpu_memory=$run.gpu_memory; managed_stages=$p.managed_stages
        native_sha256=$run.amf_native_sha256; managed_sha256=$run.amf_managed_sha256; harness_sha256=$run.bench_sha256
    }
})
$groups = @($rows | Group-Object group | ForEach-Object {
    $g=$_.Group; $r=$g[0]
    [pscustomobject]@{
        group=$_.Name; count=$g.Count; quality=$r.quality; pool=$r.pool; mode=$r.mode; profile=$r.profile; frames=$r.frames; render_repeats=$r.render_repeats
        median_fps=Median $g.fps; median_export_ms=Median $g.export_ms; min_ms=($g.export_ms | Measure-Object -Minimum).Minimum; max_ms=($g.export_ms | Measure-Object -Maximum).Maximum
        median_create_plus_export_ms=Median $g.create_plus_export_ms; median_finalize_ms=Median $g.finalize_ms
        median_run_p50_ms=Median $g.p50; median_run_p95_ms=Median $g.p95; median_run_p99_ms=Median $g.p99; worst_max_ms=($g.max | Measure-Object -Maximum).Maximum
    }
})
$report = [ordered]@{ status='validated'; scope='Isolated actual AMF DLL, synthetic D2D render, not whole YMM4 project export. No discarded outliers. CPU wall-time, not GPU timestamps.'
    runs=$rows.Count; frames=($rows.frames | Measure-Object -Sum).Sum; groups=$groups; cases=$rows }
$destination = Join-Path $root 'summary.json'
if (Test-Path -LiteralPath $destination) { throw 'Summary already exists' }
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $destination -Encoding utf8
$groups | Where-Object { -not $_.profile } | Format-Table quality,pool,render_repeats,mode,count,median_fps,median_export_ms,min_ms,max_ms
