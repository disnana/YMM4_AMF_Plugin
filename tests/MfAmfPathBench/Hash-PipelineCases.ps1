#requires -Version 7.0
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
$destination = Join-Path $root 'decoded-frame-hashes.json'
if (Test-Path -LiteralPath $destination) { throw 'Hash report already exists' }
$cases = @(foreach ($suite in @('balanced','quality','render16','render64','legacy')) {
    $suitePath = Join-Path $root $suite
    if (-not (Test-Path -LiteralPath $suitePath)) { continue }
    foreach ($case in Get-ChildItem -LiteralPath $suitePath -Directory -Filter '01-*') {
        if ((Get-Content -LiteralPath (Join-Path $case.FullName 'validation.json') -Raw | ConvertFrom-Json).status -ne 'passed') {
            throw "Validate first: $case"
        }
        [pscustomobject]@{ path=$case.FullName; suite=$suite }
    }
})
if (-not $cases.Count) { throw 'No first-round cases found' }
$hashes = @($cases | ForEach-Object -Parallel {
    $ErrorActionPreference = 'Stop'
    $output = Join-Path $_.path 'output.mp4'
    $hash = (& ffmpeg -v error -xerror -i $output -map 0:v:0 -pix_fmt yuv420p -f hash -hash sha256 - | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $hash -notmatch '^SHA256=[a-fA-F0-9]{64}$') { throw "Frame hash failed: $output" }
    [pscustomobject]@{path=[IO.Path]::GetRelativePath($using:root,$_.path);suite=$_.suite;decoded_yuv420p_sha256=$hash.Substring(7)}
} -ThrottleLimit 3)
if ($hashes.Count -ne $cases.Count) { throw 'One or more parallel hash checks failed' }
$groups = @($hashes | Group-Object suite | ForEach-Object {
    $unique = @($_.Group.decoded_yuv420p_sha256 | Sort-Object -Unique)
    [pscustomobject]@{suite=$_.Name;count=$_.Count;identical=$unique.Count -eq 1;hashes=$unique}
})
[pscustomobject]@{scope='First round, full decoded YUV420p, same quality and render load per group; not MF quality equivalence';groups=$groups;cases=$hashes} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $destination -Encoding utf8
if (@($groups | Where-Object { -not $_.identical }).Count) { throw "Decoded frames differ: $destination" }
$groups | Format-Table suite,count,identical
