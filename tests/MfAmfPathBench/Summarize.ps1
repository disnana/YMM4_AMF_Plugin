#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunRoot)
$ErrorActionPreference = 'Stop'
function Get-Stats([double[]]$Values) {
    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 0) { return $null }
    $n = $ordered.Count
    $median = if ($n % 2) { $ordered[[int][Math]::Floor($n / 2)] } else { ($ordered[$n/2-1]+$ordered[$n/2])/2 }
    [ordered]@{ count=$n; median=$median; mean=($ordered | Measure-Object -Average).Average;
        p50=$ordered[[Math]::Ceiling(.5*$n)-1]; p95=$ordered[[Math]::Ceiling(.95*$n)-1];
        p99=$ordered[[Math]::Ceiling(.99*$n)-1]; min=$ordered[0]; max=$ordered[-1] }
}
$cases = @(Get-ChildItem -LiteralPath $RunRoot -Directory | Where-Object { $_.Name -match '^(main-|control-|mf-speed-|profile-|recovery-|real-)' } | ForEach-Object {
    $run = Get-Content -LiteralPath (Join-Path $_.FullName 'run.json') -Raw | ConvertFrom-Json
    $validation = Get-Content -LiteralPath (Join-Path $_.FullName 'validation.json') -Raw | ConvertFrom-Json
    if ($validation.status -ne 'passed') { throw "Unvalidated output: $($_.Name)" }
    $group = $_.Name -replace '^(main|control|profile|recovery)-\d+-', '$1-' -replace '^mf-speed-\d+-', 'mf-speed-'
    [pscustomobject]@{ name=$_.Name; group=$group; run=$run; validation=$validation }
})
$summary = foreach ($group in ($cases | Group-Object group)) {
    $runs = @($group.Group.run)
    $stages = [ordered]@{}
    foreach ($stage in @('render_cpu','host_bitmap_copy','write_audio','write_video')) {
        $values = @($runs | ForEach-Object { $_.stages.$stage.per_frame_ms })
        $stages[$stage] = Get-Stats $values
    }
    [ordered]@{ name=$group.Name; cases=@($group.Group.name); scope=$runs[0].scope;
        export_ms=(Get-Stats $runs.export_ms); create_plus_export_ms=(Get-Stats $runs.writer_create_plus_export_ms);
        fps=(Get-Stats $runs.completed_fps); frame_delivery_ms=(Get-Stats $runs.frame_delivery_ms);
        finalize_ms=(Get-Stats $runs.finalize_ms); cpu_ms=(Get-Stats $runs.process_cpu_ms);
        allocated_bytes=(Get-Stats $runs.allocated_bytes); stages=$stages;
        pool_textures=@($runs.after_frames.bridge_TotalTextureCount | Where-Object { $null -ne $_ });
        pool_in_flight_end=@($runs.after_frames.bridge_InFlightTextureCount | Where-Object { $null -ne $_ });
        video_bitrates=@($group.Group.validation.metadata.streams | Where-Object codec_type -eq video | Select-Object -ExpandProperty bit_rate);
        audio_delta_ms=@($group.Group.validation.audio_duration_delta_ms) }
}
$report = [ordered]@{ schema_version=1; scope='Groups are separate fixtures; real-video runs include decode/upload and are quality tests, not isolated encoder speed';
    validated_cases=$cases.Count; decoded_frames=($cases.run.requested.Frames | Measure-Object -Sum).Sum;
    configurations=$summary }
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $RunRoot 'summary.json') -Encoding utf8
$summary | ForEach-Object { [pscustomobject]@{ name=$_.name; runs=$_.cases.Count;
    median_ms=[Math]::Round($_.export_ms.median,3); min_ms=[Math]::Round($_.export_ms.min,3);
    max_ms=[Math]::Round($_.export_ms.max,3); fps=[Math]::Round($_.fps.median,2);
    create_plus_export_ms=[Math]::Round($_.create_plus_export_ms.median,3) } } | Format-Table -AutoSize
