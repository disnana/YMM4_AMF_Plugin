#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$YmmDirectory,
    [Parameter(Mandatory)][string]$RunDirectory,
    [ValidateSet(4,6,8,16,32,64,128)][int[]]$Pools = @(6,8,16,32,64,128),
    [ValidateSet('legacy','recycle','wait','both')][string[]]$Modes = @('legacy','recycle','wait','both'),
    [ValidateSet('Balanced','Quality','Speed')][string]$Quality = 'Balanced',
    [ValidateRange(1,20)][int]$Rounds = 4,
    [ValidateRange(1,10000)][int]$Frames = 900,
    [int]$Width = 1920, [int]$Height = 1080,
    [switch]$Profile, [switch]$ValidateOnly,
    [ValidateRange(1,8)][int]$ValidationWorkers = 3
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
if ($ValidateOnly) {
    $pending = @(foreach ($case in Get-ChildItem -LiteralPath $root -Directory) {
        if (-not (Test-Path -LiteralPath (Join-Path $case.FullName 'run.json'))) { continue }
        $existing = Join-Path $case.FullName 'validation.json'
        if (Test-Path -LiteralPath $existing) {
            if ((Get-Content -LiteralPath $existing -Raw | ConvertFrom-Json).status -ne 'passed') { throw "Failed prior validation: $existing" }
            continue
        }
        $case.FullName
    })
    $validateScript = Join-Path $PSScriptRoot 'Validate.ps1'
    $pending | ForEach-Object -Parallel {
        $ErrorActionPreference = 'Stop'
        & $using:validateScript -RunDirectory $_
    } -ThrottleLimit $ValidationWorkers
    return
}
if (Test-Path -LiteralPath $root) { throw 'Use a new sweep directory; do not mix old/new binaries or settings' }
New-Item -ItemType Directory -Path $root | Out-Null
$cases = @(foreach ($pool in $Pools) { foreach ($mode in $Modes) { [pscustomobject]@{ Pool=$pool; Mode=$mode } } })
# Keep validation/decoding outside timing runs. Alternate the entire sequence
# so every case has a reverse-order counterpart (odd round counts are exploratory).
for ($round = 1; $round -le $Rounds; $round++) {
    $sequence = @($cases)
    if ($round % 2 -eq 0) { [Array]::Reverse($sequence) }
    foreach ($case in $sequence) {
        $name = '{0:D2}-{1}-pool{2}-{3}' -f $round,$Quality,$case.Pool,$case.Mode
        & (Join-Path $PSScriptRoot 'Run.ps1') -YmmDirectory $YmmDirectory -RunDirectory (Join-Path $root $name) `
            -Mode amf-gpu -Frames $Frames -Width $Width -Height $Height -AmfQuality $Quality -Pool $case.Pool `
            -RecycleInput ($case.Mode -in @('recycle','both')) -OutputWait ($case.Mode -in @('wait','both')) `
            -Profile $Profile.IsPresent -SkipValidation
    }
}
