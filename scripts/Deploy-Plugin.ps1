[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string]$Ymm4Directory,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'Build.ps1') -Configuration $Configuration -IncludePlugin -Ymm4Directory $Ymm4Directory

$destination = Join-Path $Ymm4Directory 'user\plugin\AMFVideoWriterPlugin'
if ($PSCmdlet.ShouldProcess($destination, 'Deploy AMFPlugin.dll and AmfNative.dll')) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'artifacts\bin\AMFPlugin.dll') -Destination $destination -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'artifacts\bin\AmfNative.dll') -Destination $destination -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') -Destination $destination -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination $destination -Force
}
