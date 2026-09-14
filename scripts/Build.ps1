[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$IncludePlugin,
    [string]$Ymm4Directory
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (-not $visualStudio) {
    throw 'Visual Studio 2022 with MSBuild and the C++ workload is required.'
}
$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'

& $msbuild (Join-Path $repositoryRoot 'RadeonBench\RadeonBench.vcxproj') /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /m
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE." }

$artifactBin = Join-Path $repositoryRoot 'artifacts\bin'
New-Item -ItemType Directory -Path $artifactBin -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot "AmfNative\bin\$Configuration\AmfNative.dll") -Destination $artifactBin -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "RadeonBench\bin\$Configuration\RadeonBench.exe") -Destination $artifactBin -Force

if ($IncludePlugin) {
    if (-not $Ymm4Directory) {
        $Ymm4Directory = Join-Path $repositoryRoot 'YukkuriMovieMaker_v4_Lite'
    }
    $requiredHostDll = Join-Path $Ymm4Directory 'YukkuriMovieMaker.Plugin.dll'
    if (-not (Test-Path -LiteralPath $requiredHostDll)) {
        throw "YMM4 SDK assemblies were not found at: $Ymm4Directory"
    }
    $normalizedYmm4 = [System.IO.Path]::GetFullPath($Ymm4Directory) + [System.IO.Path]::DirectorySeparatorChar
    & dotnet build (Join-Path $repositoryRoot 'AMFVideoWriterPlugin\AMFVideoWriterPlugin.csproj') -c $Configuration -p:Platform=x64 "-p:YMM4DirPath=$normalizedYmm4"
    if ($LASTEXITCODE -ne 0) { throw "Managed plugin build failed with exit code $LASTEXITCODE." }
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "AMFVideoWriterPlugin\bin\x64\$Configuration\net10.0-windows10.0.19041.0\AMFPlugin.dll") -Destination $artifactBin -Force
}

Write-Host "Build artifacts: $artifactBin"
