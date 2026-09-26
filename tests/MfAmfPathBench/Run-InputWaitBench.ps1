#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$YmmDirectory,
    [Parameter(Mandatory)][string]$RunDirectory,
    [ValidateSet(4,6,8,16,32,64,128)][int[]]$Pools = @(16,32),
    [ValidateRange(1,20)][int]$Rounds = 4,
    [ValidateRange(1,10000)][int]$Frames = 900
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RunDirectory)
if (Test-Path -LiteralPath $root) { throw 'Use a new input-wait benchmark directory' }
New-Item -ItemType Directory -Path $root | Out-Null
$modes = @(
    [pscustomobject]@{ Name='fixed'; Worker=$false; Signal=$false },
    [pscustomobject]@{ Name='signal'; Worker=$false; Signal=$true },
    [pscustomobject]@{ Name='worker-fixed'; Worker=$true; Signal=$false },
    [pscustomobject]@{ Name='worker-signal'; Worker=$true; Signal=$true }
)
$cases = @(foreach ($pool in $Pools) { foreach ($mode in $modes) {
    [pscustomobject]@{ Pool=$pool; Name=$mode.Name; Worker=$mode.Worker; Signal=$mode.Signal }
} })

# Timing first, serially, with whole-order reversal. Validation and profiling do
# not run concurrently with timing.
for ($round = 1; $round -le $Rounds; $round++) {
    $sequence = @($cases)
    if ($round % 2 -eq 0) { [Array]::Reverse($sequence) }
    foreach ($case in $sequence) {
        $name = '{0:D2}-pool{1}-{2}' -f $round,$case.Pool,$case.Name
        & (Join-Path $PSScriptRoot 'Run.ps1') -YmmDirectory $YmmDirectory `
            -RunDirectory (Join-Path $root "timing/$name") -Mode amf-gpu -Frames $Frames `
            -AmfQuality Balanced -Pool $case.Pool -RecycleInput $true -OutputWait $true `
            -AsyncSubmission $case.Worker -AdaptiveInputWait $case.Signal -SkipValidation
    }
}

# One diagnostic run per condition, kept out of timing medians.
foreach ($case in $cases) {
    $name = 'pool{0}-{1}' -f $case.Pool,$case.Name
    & (Join-Path $PSScriptRoot 'Run.ps1') -YmmDirectory $YmmDirectory `
        -RunDirectory (Join-Path $root "profile/$name") -Mode amf-gpu -Frames $Frames `
        -AmfQuality Balanced -Pool $case.Pool -RecycleInput $true -OutputWait $true `
        -AsyncSubmission $case.Worker -AdaptiveInputWait $case.Signal -Profile $true -SkipValidation
}

foreach ($file in Get-ChildItem -LiteralPath $root -Filter run.json -File -Recurse) {
    & (Join-Path $PSScriptRoot 'Validate.ps1') -RunDirectory $file.DirectoryName
}
