#requires -Version 7.0
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
$destination = Join-Path $root 'decoded-frame-hashes.json'
if (Test-Path -LiteralPath $destination) { throw 'Hash report already exists' }
$cases = @(foreach ($suite in @('balanced','quality')) {
    foreach ($case in Get-ChildItem -LiteralPath (Join-Path $root $suite) -Directory -Filter '01-*') {
        $validation = Get-Content -LiteralPath (Join-Path $case.FullName 'validation.json') -Raw | ConvertFrom-Json
        if ($validation.status -ne 'passed') { throw "Validate the output first: $case" }
        [pscustomobject]@{ path=$case.FullName; quality=$suite }
    }
})
if ($cases.Count -eq 0) { throw 'No first-round cases found' }
$hashes = @($cases | ForEach-Object -Parallel {
    $ErrorActionPreference = 'Stop'
    $output = Join-Path $_.path 'output.mp4'
    $hash = (& ffmpeg -v error -xerror -i $output -map 0:v:0 -pix_fmt yuv420p -f hash -hash sha256 - | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $hash -notmatch '^SHA256=[a-fA-F0-9]{64}$') { throw "Frame hash failed: $output" }
    [pscustomobject]@{ path=[IO.Path]::GetRelativePath($using:root,$_.path); quality=$_.quality; decoded_yuv420p_sha256=$hash.Substring(7) }
} -ThrottleLimit 3)
$groups = @($hashes | Group-Object quality | ForEach-Object {
    $unique = @($_.Group.decoded_yuv420p_sha256 | Sort-Object -Unique)
    [pscustomobject]@{quality=$_.Name; runs=$_.Count; unique_hashes=$unique; identical=$unique.Count -eq 1}
})
[pscustomobject]@{scope='Decoded YUV420p pixels, first round of each timing condition; same preset only, not MF quality equivalence';groups=$groups;cases=$hashes} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $destination -Encoding utf8
if (@($groups | Where-Object { -not $_.identical }).Count) { throw "Decoded frames differ; inspect $destination" }
$groups | Format-Table quality,runs,identical
