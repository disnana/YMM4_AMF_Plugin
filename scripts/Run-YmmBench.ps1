[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Profile
)

$ErrorActionPreference = 'Stop'
$profilePath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Profile))
$settings = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
if (-not $settings.executable -or -not ($settings.arguments -is [System.Array])) {
    throw 'The profile must provide executable and arguments[]. Arguments are intentionally not guessed.'
}
if (-not (Test-Path -LiteralPath $settings.executable)) { throw "YMM4 executable not found: $($settings.executable)" }

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $settings.executable
$startInfo.WorkingDirectory = if ($settings.working_directory) { $settings.working_directory } else { Split-Path -Parent $settings.executable }
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
foreach ($argument in $settings.arguments) { $startInfo.ArgumentList.Add([string]$argument) }

$process = [System.Diagnostics.Process]::Start($startInfo)
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
Write-Output $stdout
if ($stderr) { [Console]::Error.WriteLine($stderr) }
if ($process.ExitCode -ne 0) { throw "YMM4 exited with code $($process.ExitCode)." }
