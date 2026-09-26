#requires -Version 7.0
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($RunDirectory)
$destination=Join-Path $root 'decoded-frame-hashes.json'
if(Test-Path -LiteralPath $destination){throw 'Hash report already exists'}
$cases=@(Get-ChildItem -LiteralPath (Join-Path $root 'timing') -Directory -Filter '01-*'|ForEach-Object{
    $validation=Get-Content -LiteralPath (Join-Path $_.FullName 'validation.json') -Raw|ConvertFrom-Json
    $run=Get-Content -LiteralPath (Join-Path $_.FullName 'run.json') -Raw|ConvertFrom-Json
    if($validation.status -ne 'passed'){throw "Validate first: $_"}
    [pscustomobject]@{path=$_.FullName;suite="pool$($run.requested.Pool)"}
})
$hashes=@($cases|ForEach-Object -Parallel{
    $output=Join-Path $_.path 'output.mp4'
    $hash=(& ffmpeg -v error -xerror -i $output -map 0:v:0 -pix_fmt yuv420p -f hash -hash sha256 -|Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $hash -notmatch '^SHA256=[a-fA-F0-9]{64}$'){throw "Frame hash failed: $output"}
    [pscustomobject]@{path=[IO.Path]::GetRelativePath($using:root,$_.path);suite=$_.suite;decoded_yuv420p_sha256=$hash.Substring(7)}
}-ThrottleLimit 3)
$groups=@($hashes|Group-Object suite|ForEach-Object{
    $unique=@($_.Group.decoded_yuv420p_sha256|Sort-Object -Unique)
    [pscustomobject]@{suite=$_.Name;count=$_.Count;identical=$unique.Count -eq 1;hashes=$unique}
})
[pscustomobject]@{scope='First timing round, full decoded YUV420p; fixed/signal and worker modes compared only within equal pool and quality.';groups=$groups;cases=$hashes}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $destination -Encoding utf8
if(@($groups|Where-Object{-not $_.identical}).Count){throw "Decoded frames differ: $destination"}
$groups|Format-Table suite,count,identical
