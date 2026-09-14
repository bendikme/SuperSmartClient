# SPDX-License-Identifier: GPL-2.0-or-later
# Local update helper for the Windows portable app. Never execute downloaded scripts.
[CmdletBinding()]
param(
    [ValidateSet('Check', 'Download', 'Install', 'Library')][string]$Action = 'Check',
    [string]$AppDirectory,
    [string]$WorkDirectory,
    [string]$InstalledVersion,
    [int]$ParentId = 0,
    [string]$ConfigPath,
    [string]$RestartArguments,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
$script:Repository = 'https://github.com/bendikme/SuperSmartClient'
$script:PackageName = 'SuperSmartClient-windows-x64'
$script:Utf8 = [Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Net.Http

function Get-ReleaseVersion([string]$Value) {
    if ($Value -cnotmatch '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$') {
        throw 'The release has an invalid version.'
    }
    return [version]$Value
}
function Assert-PlainPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if ((Test-Path -LiteralPath $current) -and
            ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Updates cannot use directory links: $current"
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
function Get-ChildPath([string]$Root, [string]$Relative) {
    if (!$Relative -or $Relative -match '[\\<>:"|?*\x00-\x1f]' -or $Relative.StartsWith('/')) {
        throw 'The update contains an unsafe file path.'
    }
    foreach ($part in $Relative.Split('/')) {
        if (!$part -or $part -in @('.', '..') -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
            $part -match '^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|$)') {
            throw 'The update contains an unsafe file name.'
        }
    }
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $full = [IO.Path]::GetFullPath((Join-Path $base $Relative))
    if (!$full.StartsWith($base + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'An update path escapes its directory.'
    }
    Assert-PlainPath $full
    return $full
}
function Test-PackageFile([string]$Relative) {
    if ($Relative -match '(?i)\.(db|sqlite|sqlite3|sscdb)(-|$)') { return $false }
    return $Relative -cin @('SuperSmartClient.exe', 'VERSION.txt', 'SOURCE.txt', 'README.md', 'README.rst',
        'BUILDING.txt', 'LICENCE.TXT', 'build-packages.txt', 'update.ps1', 'manifest.json', 'contrib/unified/BUILDING.md') -or
        $Relative -cmatch '^[A-Za-z0-9_+.-]+\.dll$' -or $Relative -cmatch '^(fonts|licenses)/.+'
}
function Read-PackageManifest([string]$Json) {
    if ($Json.Length -gt 2MB) { throw 'The package manifest is too large.' }
    $manifest = $Json | ConvertFrom-Json
    if ($manifest.format -ne 1 -or $manifest.product -cne 'SuperSmartClient' -or $manifest.repository -cne $script:Repository) {
        throw 'This is not a SuperSmartClient release package.'
    }
    $null = Get-ReleaseVersion $manifest.version
    if (@($manifest.files).Count -lt 3 -or @($manifest.files).Count -gt 10000) { throw 'Invalid package file count.' }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [long]$total = 0
    foreach ($file in $manifest.files) {
        if (!(Test-PackageFile $file.path) -or $file.path -ieq 'manifest.json' -or !$names.Add($file.path) -or
            $file.sha256 -cnotmatch '^[a-f0-9]{64}$' -or $file.size -lt 0 -or $file.size -gt 100MB) {
            throw 'Invalid package file metadata.'
        }
        $null = Get-ChildPath $script:WorkRoot $file.path
        $total += $file.size
    }
    if ($total -gt 300MB -or !$names.Contains('SuperSmartClient.exe') -or !$names.Contains('update.ps1') -or
        !$names.Contains('VERSION.txt')) { throw 'The release package is incomplete or too large.' }
    return $manifest
}
function Write-UpdateState([string]$State, [string]$Version, [string]$Message) {
    $messageLine = ($Message -replace '[\r\n]', ' ')
    if ($messageLine.Length -gt 800) { $messageLine = $messageLine.Substring(0, 800) }
    $temporary = Get-ChildPath $script:WorkRoot 'status.tmp'
    [IO.File]::WriteAllText($temporary, "$State`n$Version`n$messageLine`n", $script:Utf8)
    Move-Item -LiteralPath $temporary -Destination (Get-ChildPath $script:WorkRoot 'status.txt') -Force
}
function Clear-UpdateDirectory([string]$Name) {
    if ($Name -cnotin @('stage', 'backup')) { throw 'Unknown generated update directory.' }
    $directory = Get-ChildPath $script:WorkRoot $Name
    if (Test-Path -LiteralPath $directory) {
        $links = @(Get-ChildItem -LiteralPath $directory -Force -Recurse | Where-Object {
            $_.Attributes -band [IO.FileAttributes]::ReparsePoint
        })
        if ($links.Count) { throw 'An update directory contains a directory link.' }
        # Keep the directory itself: Explorer can hold an otherwise empty folder open.
        Get-ChildItem -LiteralPath $directory -Force | ForEach-Object {
            $target = Get-ChildPath $directory $_.Name
            Remove-Item -LiteralPath $target -Force -Recurse
        }
    }
    $null = [IO.Directory]::CreateDirectory($directory)
    return $directory
}
function Receive-ReleaseFile([string]$Url, [string]$Destination, [long]$Limit) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [Net.Http.HttpClient]::new($handler)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd("SuperSmartClient/$InstalledVersion")
    $client.DefaultRequestHeaders.Accept.ParseAdd('application/vnd.github+json')
    $client.DefaultRequestHeaders.Add('X-GitHub-Api-Version', '2026-03-10')
    $deadline = [Threading.CancellationTokenSource]::new(90000)
    try {
        for ($redirect = 0; $redirect -lt 6; $redirect++) {
            $uri = [uri]$Url
            if ($uri.Scheme -cne 'https' -or !$uri.IsDefaultPort -or $uri.UserInfo -or
                $uri.Host -cnotin @('api.github.com', 'github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com')) {
                throw 'GitHub returned an unsupported download address.'
            }
            $response = $client.GetAsync($uri, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $deadline.Token).GetAwaiter().GetResult()
            try {
                $code = [int]$response.StatusCode
                if ($code -in @(301, 302, 303, 307, 308)) {
                    $Url = [uri]::new($uri, $response.Headers.Location).AbsoluteUri
                    continue
                }
                if ($code -eq 404) { throw 'No published Windows release is available yet.' }
                if ($code -eq 403 -or $code -eq 429) { throw 'GitHub is limiting update checks. Please try again later.' }
                if ($code -ne 200) { throw "GitHub returned HTTP $code. Please try again later." }
                if ($response.Content.Headers.ContentLength -gt $Limit) { throw 'The update download exceeds its size limit.' }
                $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
                $outputStream = [IO.File]::Create($Destination)
                try {
                    $buffer = [byte[]]::new(65536)
                    [long]$total = 0
                    while (($count = $inputStream.ReadAsync($buffer, 0, $buffer.Length, $deadline.Token).GetAwaiter().GetResult()) -gt 0) {
                        $total += $count
                        if ($total -gt $Limit) { throw 'The update download exceeds its size limit.' }
                        $outputStream.Write($buffer, 0, $count)
                    }
                } finally { $outputStream.Dispose(); $inputStream.Dispose() }
                return
            } finally { $response.Dispose() }
        }
        throw 'GitHub returned too many redirects.'
    } finally { $deadline.Dispose(); $client.Dispose(); $handler.Dispose() }
}
function Get-LatestRelease {
    $metadata = Get-ChildPath $script:WorkRoot 'release.json'
    Receive-ReleaseFile 'https://api.github.com/repos/bendikme/SuperSmartClient/releases/latest' $metadata 2MB
    $release = [IO.File]::ReadAllText($metadata) | ConvertFrom-Json
    if ($release.draft -ne $false -or $release.prerelease -ne $false -or $release.tag_name -cnotmatch '^ssc-v(.+)$') {
        throw 'The latest release is not a stable SuperSmartClient version.'
    }
    $version = $Matches[1]
    $null = Get-ReleaseVersion $version
    $assets = @($release.assets | Where-Object { $_.name -ceq "$script:PackageName.zip" })
    if ($assets.Count -ne 1) { throw 'The release does not contain one Windows package.' }
    $asset = $assets[0]
    $expected = "$script:Repository/releases/download/ssc-v$version/$script:PackageName.zip"
    if ($asset.browser_download_url -cne $expected -or $asset.state -cne 'uploaded' -or
        $asset.digest -cnotmatch '^sha256:([a-f0-9]{64})$' -or $asset.size -le 0 -or $asset.size -gt 100MB) {
        throw 'GitHub did not provide a valid package checksum and download address.'
    }
    return [pscustomobject]@{ version = $version; sha256 = $asset.digest.Substring(7); size = [long]$asset.size; url = $expected }
}
function Expand-VerifiedPackage([string]$ZipPath, [string]$Hash, [string]$Version) {
    if ((Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Hash) {
        throw 'The download checksum does not match GitHub. The update was not installed.'
    }
    $zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        if ($zip.Entries.Count -gt 12000) { throw 'Too many files in the update archive.' }
        $files = @{}
        foreach ($entry in $zip.Entries) {
            # Windows PowerShell/.NET Framework can expose ZIP separators as
            # backslashes. Canonicalise before applying traversal checks.
            $archiveName = $entry.FullName.Replace('\', '/')
            if (!$archiveName.StartsWith("$script:PackageName/", [StringComparison]::Ordinal)) { throw 'Unexpected update archive root.' }
            $relative = $archiveName.Substring($script:PackageName.Length + 1)
            if (!$relative) { continue }
            $null = Get-ChildPath $script:WorkRoot $relative.TrimEnd('/')
            if ((($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000 -or
                ($entry.ExternalAttributes -band 0x400)) { throw 'Links are not allowed in update archives.' }
            if ($archiveName.EndsWith('/')) { continue }
            if (!(Test-PackageFile $relative) -or $files.ContainsKey($relative) -or $entry.Length -gt 100MB) {
                throw 'Unexpected or duplicate file in the update archive.'
            }
            $files[$relative] = $entry
        }
        if (!$files.ContainsKey('manifest.json') -or $files['manifest.json'].Length -gt 2MB) { throw 'Missing update manifest.' }
        $reader = [IO.StreamReader]::new($files['manifest.json'].Open(), $script:Utf8)
        try { $manifestJson = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $manifest = Read-PackageManifest $manifestJson
        if ($manifest.version -cne $Version -or $files.Count -ne @($manifest.files).Count + 1) { throw 'The release and manifest do not match.' }
        foreach ($file in $manifest.files) {
            if (!$files.ContainsKey($file.path) -or $files[$file.path].Length -ne $file.size) { throw 'The update package is incomplete.' }
        }
        $stage = Clear-UpdateDirectory 'stage'
        foreach ($file in $manifest.files) {
            $destination = Get-ChildPath $stage $file.path
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($files[$file.path], $destination, $false)
            if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) {
                throw 'A file in the update package failed verification.'
            }
        }
        if ([IO.File]::ReadAllText((Get-ChildPath $stage 'VERSION.txt')).Trim() -cne $Version) { throw 'The packaged version is incorrect.' }
        [IO.File]::WriteAllText((Get-ChildPath $stage 'manifest.json'), $manifestJson, $script:Utf8)
        return $manifest
    } finally { $zip.Dispose() }
}
function Find-Update {
    $release = Get-LatestRelease
    [IO.File]::WriteAllText((Get-ChildPath $script:WorkRoot 'last-check'), $release.version, $script:Utf8)
    if ((Get-ReleaseVersion $release.version) -le (Get-ReleaseVersion $InstalledVersion)) {
        Write-UpdateState 'current' $InstalledVersion 'You are using the latest release.'
        return
    }
    $pendingPath = Get-ChildPath $script:WorkRoot 'pending.json'
    $zipPath = Get-ChildPath $script:WorkRoot 'download.zip'
    if ((Test-Path -LiteralPath $pendingPath -PathType Leaf) -and (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        try {
            $pending = [IO.File]::ReadAllText($pendingPath) | ConvertFrom-Json
            if ($pending.version -ceq $release.version -and $pending.sha256 -ceq $release.sha256 -and
                (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $release.sha256) {
                Write-UpdateState 'ready' $release.version "Version $($release.version) is ready. Install and restart when convenient."
                return
            }
        } catch { } # A stale or incomplete cache can be downloaded again.
    }
    Write-UpdateState 'available' $release.version "SuperSmartClient $($release.version) is available."
}
function Save-Update {
    $release = Get-LatestRelease
    if ((Get-ReleaseVersion $release.version) -le (Get-ReleaseVersion $InstalledVersion)) { throw 'No newer release is available.' }
    $zipPath = Get-ChildPath $script:WorkRoot 'download.zip'
    Receive-ReleaseFile $release.url $zipPath $release.size
    if ((Get-Item -LiteralPath $zipPath).Length -ne $release.size) { throw 'The download is incomplete.' }
    $null = Expand-VerifiedPackage $zipPath $release.sha256 $release.version
    [IO.File]::WriteAllText((Get-ChildPath $script:WorkRoot 'pending.json'), ($release | ConvertTo-Json), $script:Utf8)
    Write-UpdateState 'ready' $release.version "Version $($release.version) is ready. Install and restart when convenient."
}
function Install-Update {
    $pending = [IO.File]::ReadAllText((Get-ChildPath $script:WorkRoot 'pending.json')) | ConvertFrom-Json
    if ($pending.sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        (Get-ReleaseVersion $pending.version) -le (Get-ReleaseVersion $InstalledVersion)) { throw 'There is no verified newer update to install.' }
    $manifest = Expand-VerifiedPackage (Get-ChildPath $script:WorkRoot 'download.zip') $pending.sha256 $pending.version
    $current = Read-PackageManifest ([IO.File]::ReadAllText((Get-ChildPath $script:AppRoot 'manifest.json')))
    if ($current.version -cne $InstalledVersion) { throw 'The application version changed. Start the app again before updating.' }
    $running = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($script:AppRoot + '\', [StringComparison]::OrdinalIgnoreCase)
    })
    if ($running.Count) { throw 'Another copy of this app is running. Close it before installing the update.' }
    $oldNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $newNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $current.files) { $null = $oldNames.Add($file.path) }
    foreach ($file in $manifest.files) { $null = $newNames.Add($file.path) }
    $null = $oldNames.Add('manifest.json'); $null = $newNames.Add('manifest.json')
    foreach ($name in $newNames) {
        if (!$oldNames.Contains($name) -and (Test-Path -LiteralPath (Get-ChildPath $script:AppRoot $name))) {
            throw "An existing personal file would be replaced: $name"
        }
    }
    $allNames = [Collections.Generic.HashSet[string]]::new($oldNames, [StringComparer]::OrdinalIgnoreCase)
    $allNames.UnionWith($newNames)
    $backup = Clear-UpdateDirectory 'backup'
    foreach ($name in $allNames) {
        $source = Get-ChildPath $script:AppRoot $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            $destination = Get-ChildPath $backup $name
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
            Copy-Item -LiteralPath $source -Destination $destination
        }
    }
    try {
        foreach ($name in $allNames) {
            if ($name -ieq 'manifest.json') { continue }
            $destination = Get-ChildPath $script:AppRoot $name
            if ($newNames.Contains($name)) {
                $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
                Copy-Item -LiteralPath (Get-ChildPath (Get-ChildPath $script:WorkRoot 'stage') $name) -Destination $destination -Force
            } elseif (Test-Path -LiteralPath $destination -PathType Leaf) {
                Remove-Item -LiteralPath $destination -Force
            }
        }
        Copy-Item -LiteralPath (Get-ChildPath (Get-ChildPath $script:WorkRoot 'stage') 'manifest.json') -Destination (Get-ChildPath $script:AppRoot 'manifest.json') -Force
    } catch {
        $restoreErrors = @()
        foreach ($name in $allNames) {
            try {
                $original = Get-ChildPath $backup $name
                $destination = Get-ChildPath $script:AppRoot $name
                if (Test-Path -LiteralPath $original -PathType Leaf) { Copy-Item -LiteralPath $original -Destination $destination -Force }
                elseif (Test-Path -LiteralPath $destination -PathType Leaf) { Remove-Item -LiteralPath $destination -Force }
            } catch { $restoreErrors += $_ }
        }
        if ($restoreErrors.Count) { throw "The update failed and some files could not be restored. The previous files are in $backup." }
        throw 'The update could not replace the application files. The previous version has been restored.'
    }
    Write-UpdateState 'installed' $pending.version 'Update installed.'
}

if ($Action -eq 'Library') { return }
$updateLock = $null
$restart = $false
$exitCode = 0
try {
    $null = Get-ReleaseVersion $InstalledVersion
    if (!$AppDirectory -or !$WorkDirectory) { throw 'The application and update directories are required.' }
    $script:AppRoot = [IO.Path]::GetFullPath($AppDirectory).TrimEnd('\')
    $script:WorkRoot = [IO.Path]::GetFullPath($WorkDirectory).TrimEnd('\')
    Assert-PlainPath $script:AppRoot; Assert-PlainPath $script:WorkRoot
    if ($script:AppRoot -eq $script:WorkRoot -or $script:AppRoot.StartsWith($script:WorkRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The update cache must be separate from the application directory.'
    }
    $null = [IO.Directory]::CreateDirectory($script:WorkRoot)
    $updateLock = [IO.File]::Open((Get-ChildPath $script:WorkRoot 'update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    if ($Action -eq 'Check') { Find-Update }
    elseif ($Action -eq 'Download') { Save-Update }
    else {
        if ($ParentId -gt 0) {
            $parent = Get-Process -Id $ParentId -ErrorAction SilentlyContinue
            if ($parent) {
                if ($parent.Path -ine (Get-ChildPath $script:AppRoot 'SuperSmartClient.exe')) { throw 'The application process changed. Please try again.' }
                if (!$parent.WaitForExit(120000)) { throw 'The application did not close. The update was not installed.' }
            }
        }
        $restart = !$NoRestart
        Install-Update
    }
} catch {
    $exitCode = 1
    $message = $_.Exception.Message
    if ($updateLock) { Write-UpdateState 'error' '' $message }
    Write-Warning $message
} finally {
    if ($updateLock) { $updateLock.Dispose() }
}
if ($restart) {
    try {
        $arguments = @('-Dashboard=1')
        if ($ConfigPath) { $arguments += @('-DashboardConfig', ('"' + [IO.Path]::GetFullPath($ConfigPath) + '"')) }
        if ($RestartArguments) { $arguments = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($RestartArguments)) }
        # This is the interactive application explicitly requested by Install and restart.
        Start-Process -FilePath (Get-ChildPath $script:AppRoot 'SuperSmartClient.exe') -ArgumentList $arguments -WorkingDirectory $script:AppRoot -WindowStyle Normal
    } catch { Write-UpdateState 'error' '' 'The update finished but the app could not restart. Open SuperSmartClient again.'; $exitCode = 1 }
}
exit $exitCode
