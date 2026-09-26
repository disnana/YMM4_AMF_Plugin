#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$YmmDirectory,
    [Parameter(Mandatory)][string]$RunDirectory,
    [string]$NativeDll,
    [ValidateSet('mf-gpu','mf-cpu','amf-gpu','amf-cpu','reference')][string]$Mode = 'mf-gpu',
    [ValidateSet('quality','cbr','vbr')][string]$Rate = 'quality',
    [int]$Frames = 900, [int]$Width = 1920, [int]$Height = 1080, [int]$Fps = 60,
    [int]$Bitrate = 12000, [int]$Pool = 6,
    [ValidateSet('Quality','Balanced','Speed')][string]$AmfQuality = 'Quality',
    [int]$MfSpeed = 50, [int]$MfBFrames = 2,
    [bool]$RecycleInput = $false, [bool]$OutputWait = $false,
    [bool]$AsyncSubmission = $false, [bool]$DedicatedDevice = $false,
    [bool]$AdaptiveInputWait = $false, [int]$RenderRepeats = 1,
    [string]$SourceFile, [int]$SourceStart = 0,
    [bool]$Audio = $true, [bool]$MfFfmpegAudio = $true, [bool]$Profile = $false,
    [int]$TimeoutSeconds = 120, [switch]$SkipValidation, [switch]$InspectCleanup
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$appDirectory = Join-Path $PSScriptRoot 'bin/Release/net10.0-windows10.0.19041.0'
$app = Join-Path $appDirectory 'MfAmfPathBench.exe'
$caseDirectory = [IO.Path]::GetFullPath($RunDirectory)
if (-not $NativeDll) { $NativeDll = Join-Path $repo 'artifacts/bin/AmfNative.dll' }
if (Test-Path -LiteralPath $caseDirectory) { throw 'Use a new run directory' }
if ($Mode.StartsWith('mf-') -and $MfFfmpegAudio) {
    # Copy only runtime resources into generated bin, never into the actual YMM4 installation.
    $destination = [IO.Path]::GetFullPath((Join-Path $appDirectory 'Resources/bin/x64/ffmpeg'))
    if (-not $destination.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe dependency destination' }
    if (-not (Test-Path -LiteralPath (Join-Path $destination 'ffmpeg.exe'))) {
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $YmmDirectory 'Resources/bin/x64/ffmpeg') -Destination $destination -Recurse
    }
}
$info = [Diagnostics.ProcessStartInfo]::new($app)
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.WorkingDirectory = $repo
$arguments = @('--ymm',$YmmDirectory,'--amf',(Join-Path $repo 'artifacts/bin/AMFPlugin.dll'),
    '--native',[IO.Path]::GetFullPath($NativeDll),'--run',$caseDirectory,'--mode',$Mode,
    '--width',"$Width",'--height',"$Height",'--fps',"$Fps",'--frames',"$Frames",'--rate',$Rate,
    '--bitrate',"$Bitrate",'--pool',"$Pool",'--audio',$Audio.ToString().ToLowerInvariant(),
    '--mf-ffmpeg',$MfFfmpegAudio.ToString().ToLowerInvariant(),'--profile',$Profile.ToString().ToLowerInvariant(),
    '--amf-quality',$AmfQuality,'--mf-speed',"$MfSpeed",'--mf-bframes',"$MfBFrames",
    '--recycle',$RecycleInput.ToString().ToLowerInvariant(),'--output-wait',$OutputWait.ToString().ToLowerInvariant(),
    '--async-submission',$AsyncSubmission.ToString().ToLowerInvariant(),'--dedicated-device',$DedicatedDevice.ToString().ToLowerInvariant(),
    '--adaptive-input-wait',$AdaptiveInputWait.ToString().ToLowerInvariant(),
    '--render-repeats',"$RenderRepeats",
    '--source-start',"$SourceStart",'--inspect-cleanup',$InspectCleanup.IsPresent.ToString().ToLowerInvariant())
if ($SourceFile) { $arguments += @('--source',[IO.Path]::GetFullPath($SourceFile)) }
foreach ($argument in $arguments) { $info.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::Start($info)
try {
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw "Isolated benchmark timed out after $TimeoutSeconds seconds; only its process tree was stopped"
    }
    if ($process.ExitCode -ne 0) { throw "Benchmark exited with $($process.ExitCode): $caseDirectory" }
} finally { $process.Dispose() }
if (-not $SkipValidation -and $Mode -ne 'reference') { & (Join-Path $PSScriptRoot 'Validate.ps1') -RunDirectory $caseDirectory }
$run = Get-Content -LiteralPath (Join-Path $caseDirectory 'run.json') -Raw | ConvertFrom-Json
[pscustomobject]@{ case=$caseDirectory; mode=$Mode; export_ms=$run.export_ms; create_plus_export_ms=$run.writer_create_plus_export_ms; fps=$run.completed_fps; mft=$run.before.videoTransformDescription; gpu_failure=$run.after_frames.gpuDirectFailed }
