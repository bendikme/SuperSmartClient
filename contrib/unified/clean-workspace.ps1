[CmdletBinding(SupportsShouldProcess = $true)]
param()

# Remove known generated leftovers, preserving build/, the running/current
# portable app, the installed toolchain, source, and user application data.
$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$targets = @(
    'build-windows', 'build-linux', 'build-no-tls',
    'build/package-validation', 'build/package-unrelated', 'build/cleanup-plan.json',
    'tests/integration/__pycache__', '.tools/msys64/var/cache/pacman/pkg',
    'dist/siemens-tls-fix', 'dist/tls-control', 'dist/SuperSmartClient-windows-x64',
    'dist/SuperSmartClient-windows-x64.zip', 'dist/SuperSmartClient-windows-x64.zip.sha256',
    '.tools/portable smoke', '.tools/siemens-tls-portable-smoke', '.tools/tls-control-portable-smoke',
    '.tools/ui-config', '.tools/ui-state', '.tools/msys2.tar.xz',
    '.tools/capture_ui.py', '.tools/connection-dialog.bmp', '.tools/panel-presented-cert.der',
    '.tools/panel_handshake_probe.cxx', '.tools/panel_handshake_probe.exe',
    '.tools/smartclient-local-dummy.log', '.tools/smartclient-local.log', '.tools/smartclient-loopback.log'
)
$targets += @(Get-ChildItem -LiteralPath $workspaceRoot -File -Filter 'build-*.log' | ForEach-Object { $_.Name })
$targets += @(Get-ChildItem -LiteralPath (Join-Path $workspaceRoot '.tools') -File -Filter 'dashboard*.png' -ErrorAction SilentlyContinue |
    ForEach-Object { '.tools/' + $_.Name })
$running = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath } | Select-Object -ExpandProperty ExecutablePath)
$plan = foreach ($relative in $targets) {
    $candidate = Join-Path $workspaceRoot $relative
    if (!(Test-Path -LiteralPath $candidate)) { continue }
    $resolved = (Resolve-Path -LiteralPath $candidate).Path
    if (!$resolved.StartsWith($workspaceRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
        ((Get-Item -LiteralPath $resolved -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Cleanup target is outside the workspace or is a directory link: $resolved"
    }
    $tracked = & git -C $workspaceRoot ls-files -- $relative
    if ($LASTEXITCODE -ne 0 -or $tracked) { throw "Refusing to remove tracked files: $relative" }
    foreach ($exe in $running) {
        if ($exe -eq $resolved -or $exe.StartsWith($resolved + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove a running executable: $resolved"
        }
    }
    $resolved
}
$failed = @()
foreach ($target in $plan) {
    if ($PSCmdlet.ShouldProcess($target, 'Remove generated files')) {
        # An open directory handle must not prevent unrelated targets or
        # unlocked siblings from being cleaned. Report all leftovers afterward.
        $removalErrors = @()
        try {
            Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue -ErrorVariable removalErrors
        } catch {
            $removalErrors += $_
        }
        if ($removalErrors.Count -gt 0) {
            $failed += $target
            Write-Warning "Could not fully remove ${target}: $($removalErrors[0].Exception.Message)"
        }
    }
}
if ($failed.Count -gt 0) {
    throw "Cleanup finished with $($failed.Count) target(s) still present. Close programs holding the reported paths and run this script again."
}
