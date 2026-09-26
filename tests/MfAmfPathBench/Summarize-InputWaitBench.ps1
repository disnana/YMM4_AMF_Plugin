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
    $validation = Get-Content -LiteralPath (Join-Path $file.DirectoryName 'validation.json') -Raw | ConvertFrom-Json
    if ($run.status -ne 'writer_returned_validation_required' -or $validation.status -ne 'passed')
    { throw "Failed or unvalidated run: $file" }
    $r = $run.requested
    $profilePath = Join-Path $file.DirectoryName 'output.mp4.amf_profile.json'
    $p = if (Test-Path -LiteralPath $profilePath) { Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json } else { $null }
    if ($p) {
        $inputWait = $p.input_full_wait
        if (-not $p.output_finalized -or $p.status -ne 'writer_disposed' -or
            $inputWait.requested -ne $r.AdaptiveInputWait -or
            $p.pipeline.async_submission -ne $r.AsyncSubmission -or
            $p.pipeline.submitted_frames -ne $r.Frames -or $p.pipeline.queue_depth -ne 0 -or
            $p.native.accepted_frames -ne $r.Frames -or $p.native.completed_frames -ne $r.Frames -or
            $p.input_recycling.invalid_events -ne 0) { throw "Input-wait lifecycle mismatch: $profilePath" }
        if ($r.AdaptiveInputWait -and ($inputWait.waits -ne $p.native.input_retries -or
            $inputWait.signaled_waits + $inputWait.timed_out_waits -ne $inputWait.waits))
        { throw "Input-wait counter mismatch: $profilePath" }
        if (-not $r.AdaptiveInputWait -and ($inputWait.effective_mode -ne 'sleep_1ms_legacy' -or $inputWait.waits -ne 0))
        { throw "Legacy wait changed: $profilePath" }
    }
    $mode = if ($r.AsyncSubmission) { if ($r.AdaptiveInputWait) {'worker-signal'} else {'worker-fixed'} }
        elseif ($r.AdaptiveInputWait) {'signal'} else {'fixed'}
    [pscustomobject]@{
        path=[IO.Path]::GetRelativePath($root,$file.DirectoryName); group="pool$($r.Pool)-$mode-profile$($r.Profile)"
        pool=$r.Pool; mode=$mode; profile=$r.Profile; frames=$r.Frames; fps=$run.completed_fps
        export_ms=$run.export_ms; finalize_ms=$run.finalize_ms; cpu_ms=$run.process_cpu_ms
        allocations=$run.allocated_bytes; gc=$run.gc_collections
        p50=$run.stages.write_video.p50_ms; p95=$run.stages.write_video.p95_ms
        p99=$run.stages.write_video.p99_ms; max=$run.stages.write_video.max_ms
        input_wait=$p.input_full_wait; native=$p.native; pipeline=$p.pipeline; pool_status=$p.texture_pool
        native_sha256=$run.amf_native_sha256; managed_sha256=$run.amf_managed_sha256; harness_sha256=$run.bench_sha256
    }
})
$groups = @($rows | Group-Object group | ForEach-Object {
    $g=$_.Group; $r=$g[0]
    [pscustomobject]@{
        group=$_.Name; count=$g.Count; pool=$r.pool; mode=$r.mode; profile=$r.profile; frames=$r.frames
        median_fps=Median $g.fps; median_export_ms=Median $g.export_ms
        min_ms=($g.export_ms|Measure-Object -Minimum).Minimum; max_ms=($g.export_ms|Measure-Object -Maximum).Maximum
        median_finalize_ms=Median $g.finalize_ms; median_cpu_ms=Median $g.cpu_ms
        median_p50_ms=Median $g.p50; median_p95_ms=Median $g.p95; median_p99_ms=Median $g.p99
        worst_frame_ms=($g.max|Measure-Object -Maximum).Maximum
    }
})
$report=[ordered]@{
    status='validated'; scope='Actual isolated AMF DLL; fixed 1ms sleep versus encoded-output notification with 10ms fallback. No discarded outliers. Not whole-YMM4 export.'
    runs=$rows.Count; frames=($rows.frames|Measure-Object -Sum).Sum; groups=$groups; cases=$rows
}
$destination=Join-Path $root 'summary.json'
if(Test-Path -LiteralPath $destination){throw 'Summary already exists'}
$report|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $destination -Encoding utf8
$groups|Where-Object{-not $_.profile}|Format-Table pool,mode,count,median_fps,median_export_ms,min_ms,max_ms
