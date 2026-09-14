param(
    [string]$BuildDir = 'build',
    [Parameter(Mandatory = $true)][string]$RuntimeBin,
    [string]$OutputDir = 'dist/workspaces',
    [switch]$Replace,
    [switch]$ZipOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildRoot = (Resolve-Path -LiteralPath $BuildDir).Path
$runtimeRoot = (Resolve-Path -LiteralPath $RuntimeBin).Path
$objdump = Join-Path $runtimeRoot 'objdump.exe'
$viewer = Join-Path $buildRoot 'vncviewer\vncviewer.exe'
if (!(Test-Path -LiteralPath $viewer) -or !(Test-Path -LiteralPath $objdump)) {
    throw 'The compiled viewer and MinGW objdump.exe are required.'
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDir).Path
$packageRoot = $outputRoot
if ($ZipOnly) {
    $packageRoot = Join-Path $buildRoot 'package'
    New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
    $packageRoot = (Resolve-Path -LiteralPath $packageRoot).Path
    if (!$packageRoot.StartsWith($buildRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
        ((Get-Item -LiteralPath $packageRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'ZIP staging must remain inside the build directory.'
    }
}
$package = Join-Path $packageRoot 'SuperSmartClient-windows-x64'
if (Test-Path -LiteralPath $package) {
    if (!$Replace) { throw "Package already exists. Close the app and use -Replace to refresh this same directory: $package" }
    $resolvedPackage = (Resolve-Path -LiteralPath $package).Path
    $expectedPackage = [IO.Path]::GetFullPath((Join-Path $packageRoot 'SuperSmartClient-windows-x64'))
    if ($resolvedPackage -ne $expectedPackage -or
        ((Get-Item -LiteralPath $resolvedPackage -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Refusing to replace a package outside the expected output directory or through a directory link.'
    }
    $sourceFile = Join-Path $resolvedPackage 'SOURCE.txt'
    if (!(Test-Path -LiteralPath $sourceFile) -or
        !(Select-String -LiteralPath $sourceFile -SimpleMatch -Quiet 'SuperSmartClient source: https://github.com/bendikme/SuperSmartClient')) {
        throw 'Refusing to replace a directory that is not a generated SuperSmartClient package.'
    }
    $running = Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($resolvedPackage + '\', [StringComparison]::OrdinalIgnoreCase)
    }
    if ($running) { throw "The packaged app is running. Close it before replacing $resolvedPackage" }
    Remove-Item -LiteralPath $resolvedPackage -Recurse -Force
}
New-Item -ItemType Directory -Path $package | Out-Null
Copy-Item -LiteralPath $viewer -Destination (Join-Path $package 'SuperSmartClient.exe')
Copy-Item -LiteralPath (Join-Path $repoRoot 'media\fonts\roboto') -Destination (Join-Path $package 'fonts') -Recurse

# Resolve transitive DLL imports. System DLLs are supplied by Windows.
$pending = [System.Collections.Generic.Queue[string]]::new()
$seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$pending.Enqueue($viewer)
while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    $imports = & $objdump -p $binary
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect $binary" }
    foreach ($line in $imports) {
        if ($line -match 'DLL Name:\s+(\S+)') {
            $dll = $Matches[1]
            if (!$seen.Add($dll)) { continue }
            $dependency = Join-Path $runtimeRoot $dll
            if (Test-Path -LiteralPath $dependency) {
                Copy-Item -LiteralPath $dependency -Destination $package
                $pending.Enqueue($dependency)
            } elseif (!(Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$dll")) -and
                      $dll -notmatch '^(api-ms-|ext-ms-)') {
                throw "Missing runtime dependency: $dll"
            }
        }
    }
}

Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination $package
Copy-Item -LiteralPath (Join-Path $repoRoot 'README.rst') -Destination $package
Copy-Item -LiteralPath (Join-Path $repoRoot 'BUILDING.txt') -Destination $package
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENCE.TXT') -Destination $package
$buildDocs = Join-Path $package 'contrib\unified'
New-Item -ItemType Directory -Path $buildDocs -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'BUILDING.md') -Destination $buildDocs
$licenses = Join-Path (Split-Path $runtimeRoot) 'share\licenses'
if (Test-Path -LiteralPath $licenses) {
    Copy-Item -LiteralPath $licenses -Destination (Join-Path $package 'licenses') -Recurse
}
$sourceCommit = & git -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the source revision.' }
@"
SuperSmartClient source: https://github.com/bendikme/SuperSmartClient
Revision: $sourceCommit
Upstream base: https://github.com/TigerVNC/tigervnc/tree/v1.16.2

Build recipe: contrib/unified/BUILDING.md in the source repository.
Runtime libraries were obtained from MSYS2's mingw64 packages.
Source recipes: https://github.com/msys2/MINGW-packages
Package/source links: https://packages.msys2.org
Library license notices are included in the licenses directory.
"@ | Set-Content -LiteralPath (Join-Path $package 'SOURCE.txt') -Encoding UTF8

$pacman = Join-Path (Split-Path (Split-Path $runtimeRoot)) 'usr\bin\pacman.exe'
if (Test-Path -LiteralPath $pacman) {
    & $pacman -Q | Set-Content -LiteralPath (Join-Path $package 'build-packages.txt') -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record build dependencies.' }
}
$zip = Join-Path $outputRoot 'SuperSmartClient-windows-x64.zip'
Compress-Archive -LiteralPath $package -DestinationPath $zip -Force
Get-FileHash -Algorithm SHA256 -LiteralPath $zip |
    ForEach-Object { "$($_.Hash.ToLower())  SuperSmartClient-windows-x64.zip" } |
    Set-Content -LiteralPath "$zip.sha256" -Encoding ASCII
Write-Output $zip
