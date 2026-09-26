#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$root = [IO.Path]::GetFullPath($RunDirectory)
if (Test-Path -LiteralPath $root) { throw 'Use a new native validation directory' }
New-Item -ItemType Directory -Path $root | Out-Null
function Invoke-BoundedNative([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $repo 'artifacts/bin/RadeonBench.exe'))
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.WorkingDirectory = $repo
    foreach ($arg in $Arguments) { $info.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        if (-not $process.WaitForExit(30000)) {
            $process.Kill($true); $process.WaitForExit()
            throw 'Native test timed out; only isolated test process was stopped'
        }
        if ($process.ExitCode -ne 0) { throw "Native test failed: $($Arguments -join ' ')" }
    } finally { $process.Dispose() }
}
Invoke-BoundedNative @('pipeline-contract-test','--output',(Join-Path $root 'contracts/output'))
foreach ($codec in @('h264','hevc')) {
    foreach ($recycle in @($false,$true)) {
      foreach ($adaptive in @($false,$true)) {
        foreach ($mode in @('base','worker','device','both')) {
            $name = "$codec-recycle$recycle-adaptive$adaptive-$mode"
            $case = Join-Path $root $name
            New-Item -ItemType Directory -Path $case | Out-Null
            $output = Join-Path $case 'output.mp4'; $result = Join-Path $case 'run.json'
            $extra = @('--profile','--no-debug-log','--optimize-output-wait')
            if ($recycle) { $extra += '--recycle-input' }
            if ($adaptive) { $extra += '--adaptive-input-wait' }
            if ($mode -in @('worker','both')) { $extra += '--async-submission' }
            if ($mode -in @('device','both')) { $extra += '--dedicated-device' }
            Invoke-BoundedNative (@('run','--output',$output,'--result',$result,'--codec',$codec,
                '--width','640','--height','360','--fps','60','--frames','120','--pool-size','16','--audio') + $extra)
            & (Join-Path $repo 'scripts/Validate-Output.ps1') -Output $output -Result $result `
                -Codec $codec -Width 640 -Height 360 -Fps 60 -ExpectedFrames 120 -ExpectAudio
            Invoke-BoundedNative (@('output-wait-stop-test','--pool-size','32','--output',(Join-Path $case 'abort')) + $extra)
        }
      }
    }
}
