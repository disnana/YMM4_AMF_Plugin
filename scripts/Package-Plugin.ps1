[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory)]
    [string]$Ymm4Directory,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$artifactsRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'artifacts'))
$versionFile = Join-Path $repositoryRoot 'VERSION'

if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw 'VERSION was not found.'
}

$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
$semVerPattern = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$'
if ($version -notmatch $semVerPattern) {
    throw "VERSION must contain a valid SemVer value: $version"
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $artifactsRoot 'release'
}
$releaseRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$stagingRoot = Join-Path $artifactsRoot 'ymme-staging'
$pluginFolder = Join-Path $stagingRoot 'AMFVideoWriterPlugin'
$packageName = "YMM4-Radeon-AMF-v$version.ymme"
$packagePath = Join-Path $releaseRoot $packageName
$zipPath = Join-Path $releaseRoot ([System.IO.Path]::ChangeExtension($packageName, '.zip'))

function Assert-UnderArtifacts([string]$Path) {
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $prefix = $artifactsRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside artifacts: $resolved"
    }
}

Assert-UnderArtifacts $releaseRoot
Assert-UnderArtifacts $stagingRoot

& (Join-Path $PSScriptRoot 'Build.ps1') -Configuration $Configuration -IncludePlugin -Ymm4Directory $Ymm4Directory
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

foreach ($path in @($stagingRoot, $releaseRoot)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}

New-Item -ItemType Directory -Path $pluginFolder -Force | Out-Null
New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null

$packageFiles = @(
    @{ Source = (Join-Path $artifactsRoot 'bin\AMFPlugin.dll'); Name = 'AMFPlugin.dll' },
    @{ Source = (Join-Path $artifactsRoot 'bin\AmfNative.dll'); Name = 'AmfNative.dll' },
    @{ Source = (Join-Path $repositoryRoot 'README.md'); Name = 'README.md' },
    @{ Source = (Join-Path $repositoryRoot 'LICENSE'); Name = 'LICENSE' },
    @{ Source = (Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.txt'); Name = 'THIRD_PARTY_NOTICES.txt' },
    @{ Source = $versionFile; Name = 'VERSION' }
)

foreach ($file in $packageFiles) {
    if (-not (Test-Path -LiteralPath $file.Source -PathType Leaf)) {
        throw "Package input was not found: $($file.Source)"
    }
    Copy-Item -LiteralPath $file.Source -Destination (Join-Path $pluginFolder $file.Name) -Force
}

Compress-Archive -LiteralPath $pluginFolder -DestinationPath $zipPath -CompressionLevel Optimal
Move-Item -LiteralPath $zipPath -Destination $packagePath

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
try {
    $entries = @($archive.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) } | ForEach-Object { $_.FullName.Replace('\', '/') })
    $expected = @($packageFiles | ForEach-Object { "AMFVideoWriterPlugin/$($_.Name)" })
    $missing = @($expected | Where-Object { $_ -notin $entries })
    $unexpected = @($entries | Where-Object { $_ -notin $expected })
    if ($missing.Count -gt 0 -or $unexpected.Count -gt 0) {
        throw "Invalid package contents. Missing=[$($missing -join ', ')] Unexpected=[$($unexpected -join ', ')]"
    }
    if ($entries | Where-Object { $_ -match '(^|/)YukkuriMovieMaker\..*\.dll$' }) {
        throw 'YMM4 host assemblies must not be redistributed in the package.'
    }
}
finally {
    $archive.Dispose()
}

$hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$packagePath.sha256"
Set-Content -LiteralPath $checksumPath -Value "$hash  $packageName" -Encoding ascii

Write-Host "Package: $packagePath"
Write-Host "SHA256: $hash"
Write-Output $packagePath
