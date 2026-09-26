#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$YmmDirectory,
    [Parameter(Mandatory)][string]$RunDirectory,
    [ValidateSet(4,6,8,16,32,64,128)][int[]]$Pools = @(16,32),
    [ValidateSet('Balanced','Quality','Speed')][string]$Quality = 'Balanced',
    [ValidateRange(1,20)][int]$Rounds = 4,
    [ValidateRange(1,10000)][int]$Frames = 900,
    [ValidateRange(1,64)][int]$RenderRepeats = 1,
    [bool]$RecycleInput = $true, [bool]$OutputWait = $true,
    [switch]$Profile, [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
if ($ValidateOnly) {
    & (Join-Path $PSScriptRoot 'Run-PoolSweep.ps1') -YmmDirectory $YmmDirectory -RunDirectory $root -ValidateOnly
    # Parallel validators may emit nonterminating job errors: audit the result files too.
    foreach ($case in Get-ChildItem -LiteralPath $root -Directory) {
        $validation = Join-Path $case.FullName 'validation.json'
        if ((Get-Content -LiteralPath $validation -Raw | ConvertFrom-Json).status -ne 'passed') { throw "Invalid output: $case" }
    }
    return
}
if (Test-Path -LiteralPath $root) { throw 'Use a new pipeline sweep directory' }
New-Item -ItemType Directory -Path $root | Out-Null
$cases = @(foreach ($pool in $Pools) { foreach ($mode in @('base','worker','device','both')) { [pscustomobject]@{ Pool=$pool; Mode=$mode } } })
# All timing runs serial; never validate/decode concurrently. Reverse the whole
# sequence each second round, with all codec/quality/bitrate/pool options fixed.
for ($round = 1; $round -le $Rounds; $round++) {
    $sequence = @($cases)
    if ($round % 2 -eq 0) { [Array]::Reverse($sequence) }
    foreach ($case in $sequence) {
        $name = '{0:D2}-{1}-pool{2}-{3}' -f $round,$Quality,$case.Pool,$case.Mode
        & (Join-Path $PSScriptRoot 'Run.ps1') -YmmDirectory $YmmDirectory -RunDirectory (Join-Path $root $name) `
            -Mode amf-gpu -Frames $Frames -AmfQuality $Quality -Pool $case.Pool -RenderRepeats $RenderRepeats `
            -RecycleInput $RecycleInput -OutputWait $OutputWait -AsyncSubmission ($case.Mode -in @('worker','both')) `
            -DedicatedDevice ($case.Mode -in @('device','both')) -Profile $Profile.IsPresent -SkipValidation
    }
}
