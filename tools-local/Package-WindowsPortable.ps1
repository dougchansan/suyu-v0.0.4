param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,

    [Parameter(Mandatory = $true)]
    [string]$RuntimeLicensePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$RepositoryPath = (Get-Location).Path,

    [string]$Version = '0.0.4.1'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw 'Version must contain four numeric components.'
}

$source = (Get-Item -LiteralPath $PackagePath).FullName
$licenses = (Get-Item -LiteralPath $RuntimeLicensePath).FullName
$repository = (Get-Item -LiteralPath $RepositoryPath).FullName
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$packageName = "suyu-v$Version-windows-x64-portable"
$staging = Join-Path $output $packageName
$archive = Join-Path $output "$packageName.zip"
$checksumPath = Join-Path $output "$packageName.zip.sha256"
if ((Test-Path -LiteralPath $staging) -or (Test-Path -LiteralPath $archive) -or
    (Test-Path -LiteralPath $checksumPath)) {
    throw 'Release output already exists. Use a new output directory.'
}
foreach ($required in 'suyu.exe', 'qt.conf', 'plugins\platforms\qwindows.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) {
        throw "Missing package file: $required"
    }
}

$null = New-Item -ItemType Directory -Path $staging
$null = New-Item -ItemType Directory -Path (Join-Path $staging 'user')
Copy-Item -LiteralPath (Join-Path $source 'suyu.exe'),(Join-Path $source 'qt.conf') -Destination $staging
Get-ChildItem -LiteralPath $source -Filter '*.dll' -File | Copy-Item -Destination $staging
$pluginRoot = Join-Path $source 'plugins'
foreach ($plugin in (Get-ChildItem -LiteralPath $pluginRoot -Filter '*.dll' -File -Recurse)) {
    $relativePath = $plugin.FullName.Substring($source.Length + 1)
    $destination = Join-Path $staging $relativePath
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
    Copy-Item -LiteralPath $plugin.FullName -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $repository 'LICENSE.txt') -Destination $staging
Copy-Item -LiteralPath (Join-Path $repository 'LICENSES') -Destination $staging -Recurse
Copy-Item -LiteralPath $licenses -Destination (Join-Path $staging 'runtime-licenses') -Recurse

$originalHash = (Get-FileHash -LiteralPath (Join-Path $source 'suyu.exe') -Algorithm SHA256).Hash
$stagedHash = (Get-FileHash -LiteralPath (Join-Path $staging 'suyu.exe') -Algorithm SHA256).Hash
if ($originalHash -ne $stagedHash) { throw 'Staged executable does not match the tested build.' }

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $staging, $archive, [System.IO.Compression.CompressionLevel]::Optimal, $true
)
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
try {
    $userDirectory = "$packageName/user/"
    $hasUserDirectory = $false
    foreach ($entry in $zip.Entries) {
        $entryPath = $entry.FullName.Replace('\', '/')
        if ($entryPath -eq $userDirectory) { $hasUserDirectory = $true }
        if ($entryPath.StartsWith($userDirectory) -and $entryPath -ne $userDirectory) {
            throw 'Archive unexpectedly contains user data.'
        }
        if ($entryPath -match '(?i)\.(dmp|keys|nsp|xci|nca|log)$') {
            throw 'Archive contains a forbidden user-data or diagnostic file.'
        }
    }
    if (-not $hasUserDirectory) {
        throw 'Archive is missing the empty portable user directory.'
    }
    Write-Output "Archive verified: $($zip.Entries.Count) entries; portable user directory is empty."
} finally {
    $zip.Dispose()
}
$digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText(
    $checksumPath, "$digest  $packageName.zip`n", (New-Object System.Text.UTF8Encoding($false))
)
[PSCustomObject]@{ Archive = $archive; SHA256 = $digest; ExecutableSHA256 = $originalHash }