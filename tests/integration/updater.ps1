# SPDX-License-Identifier: GPL-2.0-or-later
# Exercises the real update helper against generated packages; no network or GUI.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repo 'contrib\unified\update.ps1') -Action Library
$testRoot = [IO.Path]::GetFullPath((Join-Path $repo 'build\update-tests'))
if (!$testRoot.StartsWith($repo + '\build\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test output directory.' }
Assert-PlainPath $testRoot
$null = [IO.Directory]::CreateDirectory($testRoot)
$script:Passed = 0

function Assert-True($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Assert-Rejected([scriptblock]$Operation) {
    $rejected = $false
    try { & $Operation | Out-Null } catch { $rejected = $true }
    Assert-True $rejected 'Unsafe or invalid update was accepted.'
}
function Write-TestPackage([string]$Directory, [string]$Version, [hashtable]$Extra = @{}) {
    $null = [IO.Directory]::CreateDirectory($Directory)
    $contents = @{'SuperSmartClient.exe'="app-$Version"; 'VERSION.txt'=$Version; 'update.ps1'="# helper-$Version"}
    foreach ($key in $Extra.Keys) { $contents[$key] = $Extra[$key] }
    $files = @()
    foreach ($name in ($contents.Keys | Sort-Object)) {
        $file = Join-Path $Directory $name
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file))
        [IO.File]::WriteAllText($file, $contents[$name], $script:Utf8)
        $files += @{ path=$name; size=(Get-Item -LiteralPath $file).Length; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $manifest = @{format=1; product='SuperSmartClient'; repository=$script:Repository; version=$Version; files=$files}
    [IO.File]::WriteAllText((Join-Path $Directory 'manifest.json'), ($manifest | ConvertTo-Json -Depth 6), $script:Utf8)
}
function Set-TestRelease([string]$Version = '1.1.0') {
    $hash = (Get-FileHash -LiteralPath $script:SourceZip -Algorithm SHA256).Hash.ToLowerInvariant()
    $script:Release = @{ draft=$false; prerelease=$false; tag_name="ssc-v$Version"; assets=@(@{
        name="$script:PackageName.zip"; state='uploaded'; size=(Get-Item -LiteralPath $script:SourceZip).Length;
        digest="sha256:$hash"; browser_download_url="$script:Repository/releases/download/ssc-v$Version/$script:PackageName.zip"
    }) }
}
function Receive-ReleaseFile([string]$Url, [string]$Destination, [long]$Limit) {
    $script:Requests += $Url
    if ($Url -eq 'https://api.github.com/repos/bendikme/SuperSmartClient/releases/latest') {
        [IO.File]::WriteAllText($Destination, ($script:Release | ConvertTo-Json -Depth 6), $script:Utf8)
    } elseif ($Url -eq $script:Release.assets[0].browser_download_url) {
        Copy-Item -LiteralPath $script:SourceZip -Destination $Destination -Force
    } else { throw 'The updater requested an unexpected URL.' }
}
function Assert-PersonalData {
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'dashboard.db')) -ceq 'saved layouts and protected credentials') 'Application database was changed.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'my-layouts.sscdb')) -ceq 'encrypted export') 'Export was changed.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'notes.txt')) -ceq 'operator notes') 'Personal file was changed.'
}
function Test-Update([string]$Name, [scriptblock]$Body) {
    $case = Join-Path $testRoot ([guid]::NewGuid().ToString('N') + ' Norwegian ' + [char]0xf8)
    $script:WorkRoot = Join-Path $case 'cache'
    $script:AppRoot = Join-Path $case 'portable app'
    $script:InstalledVersion = '1.0.0'
    $script:Requests = @()
    $script:SourceZip = Join-Path $case 'source.zip'
    $script:Package = Join-Path $case "release\$script:PackageName"
    $null = [IO.Directory]::CreateDirectory($script:WorkRoot)
    try {
        Write-TestPackage $script:AppRoot '1.0.0' @{'obsolete.dll'='old library'}
        [IO.File]::WriteAllText((Join-Path $script:AppRoot 'dashboard.db'), 'saved layouts and protected credentials')
        [IO.File]::WriteAllText((Join-Path $script:AppRoot 'my-layouts.sscdb'), 'encrypted export')
        [IO.File]::WriteAllText((Join-Path $script:AppRoot 'notes.txt'), 'operator notes')
        Write-TestPackage $script:Package '1.1.0' @{'new.dll'='new library'; 'fonts/Roboto.ttf'='font fixture'}
        Compress-Archive -LiteralPath $script:Package -DestinationPath $script:SourceZip
        Set-TestRelease
        & $Body
        Assert-PersonalData
        $script:Passed++
        Write-Output "PASS: $Name"
    } finally {
        $resolved = [IO.Path]::GetFullPath($case)
        if (!$resolved.StartsWith($testRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid fixture cleanup path.' }
        Assert-PlainPath $resolved
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

Test-Update 'Automatic check does not download or replace application files' {
    Find-Update
    Assert-True ($script:Requests.Count -eq 1) 'A check downloaded an archive.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:WorkRoot 'status.txt')).StartsWith("available`n1.1.0`n")) 'Update was not offered.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'SuperSmartClient.exe')) -ceq 'app-1.0.0') 'A check changed the app.'
}
Test-Update 'Numeric version comparison handles double digit components and refuses malformed versions' {
    $script:InstalledVersion = '1.10.0'
    Find-Update
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:WorkRoot 'status.txt')).StartsWith("current`n")) 'Updater offered a downgrade.'
    foreach ($invalid in @('1.0', 'v1.0.0', '1.0.0-beta', '1.0.0.1', '01.0.0', '1.0.0;cmd')) { Assert-Rejected { Get-ReleaseVersion $invalid } }
}
Test-Update 'Draft, prerelease, foreign URL, missing checksum and duplicate assets are rejected' {
    foreach ($field in @('draft', 'prerelease')) { Set-TestRelease; $script:Release[$field] = $true; Assert-Rejected { Get-LatestRelease } }
    Set-TestRelease; $script:Release.assets[0].browser_download_url = 'https://example.com/malicious.zip'; Assert-Rejected { Get-LatestRelease }
    Set-TestRelease; $script:Release.assets[0].digest = $null; Assert-Rejected { Get-LatestRelease }
    Set-TestRelease; $script:Release.assets += $script:Release.assets[0]; Assert-Rejected { Get-LatestRelease }
}
Test-Update 'Download is verified and waits for explicit installation' {
    Save-Update
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:WorkRoot 'status.txt')).StartsWith("ready`n1.1.0`n")) 'Downloaded package was not ready.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'SuperSmartClient.exe')) -ceq 'app-1.0.0') 'Download installed without user action.'
    Find-Update
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:WorkRoot 'status.txt')).StartsWith("ready`n1.1.0`n")) 'A recheck forgot the downloaded update.'
}
Test-Update 'Checksum mismatch rejects the package' {
    $script:Release.assets[0].digest = 'sha256:' + ('0' * 64)
    Assert-Rejected { Save-Update }
    Assert-True (!(Test-Path -LiteralPath (Join-Path $script:WorkRoot 'pending.json'))) 'Invalid update was marked ready.'
}
Test-Update 'Traversal, reserved names, alternate streams and absolute paths are rejected' {
    foreach ($name in @('../escape', '/escape', 'fonts/../../escape', 'C:/escape', 'fonts\escape', 'fonts/a:stream', 'fonts/CON.txt', 'fonts/a.')) {
        Assert-Rejected { Get-ChildPath $script:WorkRoot $name }
    }
    $zip = [IO.Compression.ZipFile]::Open($script:SourceZip, 'Update')
    try { $null = $zip.CreateEntry("$script:PackageName/../escape.exe") } finally { $zip.Dispose() }
    Set-TestRelease
    Assert-Rejected { Save-Update }
}
Test-Update 'Unexpected database and case-insensitive duplicate archive entries are rejected' {
    $zip = [IO.Compression.ZipFile]::Open($script:SourceZip, 'Update')
    try { $null = $zip.CreateEntry("$script:PackageName/dashboard.db") } finally { $zip.Dispose() }
    Set-TestRelease; Assert-Rejected { Save-Update }
    $zip = [IO.Compression.ZipFile]::Open($script:SourceZip, 'Update')
    try {
        ($zip.Entries | Where-Object { $_.FullName.Replace('\', '/') -ceq "$script:PackageName/dashboard.db" }).Delete()
        $null = $zip.CreateEntry("$script:PackageName/VERSION.TXT")
    } finally { $zip.Dispose() }
    Set-TestRelease; Assert-Rejected { Save-Update }
}
Test-Update 'Install replaces only managed files, removes obsolete libraries and keeps a backup' {
    Save-Update; Install-Update
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'SuperSmartClient.exe')) -ceq 'app-1.1.0') 'New app was not installed.'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $script:AppRoot 'obsolete.dll'))) 'Obsolete library survived.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:WorkRoot 'backup/SuperSmartClient.exe')) -ceq 'app-1.0.0') 'Previous app was not backed up.'
}
Test-Update 'Tampering after download is detected before installation' {
    Save-Update
    [IO.File]::AppendAllText((Join-Path $script:WorkRoot 'download.zip'), 'tamper')
    Assert-Rejected { Install-Update }
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'SuperSmartClient.exe')) -ceq 'app-1.0.0') 'Tampered update changed the app.'
}
Test-Update 'A collision with a personal file leaves the installation untouched' {
    [IO.File]::WriteAllText((Join-Path $script:AppRoot 'new.dll'), 'personal file')
    Save-Update; Assert-Rejected { Install-Update }
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'new.dll')) -ceq 'personal file') 'Personal file was overwritten.'
}
Test-Update 'A partial installation failure restores the previous application' {
    Save-Update
    $script:InjectedFailure = $false
    function Copy-Item {
        param($LiteralPath, $Destination, [switch]$Force)
        if (!$script:InjectedFailure -and $Destination -eq (Join-Path $script:AppRoot 'SuperSmartClient.exe')) {
            [IO.File]::WriteAllText($Destination, 'partially replaced executable')
            $script:InjectedFailure = $true
            throw 'Simulated disk write failure'
        }
        Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force
    }
    try { Assert-Rejected { Install-Update } } finally { Remove-Item -LiteralPath Function:\Copy-Item }
    Assert-True $script:InjectedFailure 'The rollback fault was not injected.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'SuperSmartClient.exe')) -ceq 'app-1.0.0') 'Previous app was not restored.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'VERSION.txt')) -ceq '1.0.0') 'Previous version marker was not restored.'
}
Test-Update 'The actual helper installs from paths with spaces and Unicode without launching a viewer' {
    Save-Update
    & (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $repo 'contrib\unified\update.ps1') -Action Install -AppDirectory $script:AppRoot -WorkDirectory $script:WorkRoot -InstalledVersion '1.0.0' -NoRestart
    Assert-True ($LASTEXITCODE -eq 0) 'The standalone helper failed.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $script:AppRoot 'SuperSmartClient.exe')) -ceq 'app-1.1.0') 'Standalone helper did not install the update.'
}
Write-Output "All $script:Passed updater tests passed. No network requests, application launches or panel input."
