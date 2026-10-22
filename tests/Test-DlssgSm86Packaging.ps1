param([Parameter(Mandatory)][string]$RuntimeDirectory)
$ErrorActionPreference = 'Stop'
$scripts = Join-Path $PSScriptRoot '../scripts'
. (Join-Path $scripts 'DlssgSm86Package.ps1')
$contract = Assert-DlssgSm86Files $RuntimeDirectory
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryRoot ('Magpie DLSSG 测试 ' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $fixture 'package with spaces'
$dependency = Join-Path $fixture 'dependency'
$checks = 0
function Expect-Rejection([scriptblock]$Action) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (!$rejected) { throw 'Expected integrity rejection.' }
    $script:checks++
}
function Restore-Package {
    & (Join-Path $scripts 'Copy-DlssgSm86Runtime.ps1') -RuntimeDirectory $RuntimeDirectory -OutputDirectory $package | Out-Null
}
try {
    New-Item -ItemType Directory -Path $package,$dependency -Force | Out-Null
    Restore-Package
    $checks++
    $runtime = Join-Path $package 'runtimes/dlssg_sm86'
    foreach ($record in $contract.Files) {
        $file = Join-Path $runtime $record.Name
        Remove-Item -LiteralPath $file
        Expect-Rejection { & (Join-Path $scripts 'Test-DlssgSm86Package.ps1') -PackageDirectory $package }
        Restore-Package
        # Same-length corruption exercises hashes independently of file size.
        $bytes = [IO.File]::ReadAllBytes($file)
        $bytes[0] = $bytes[0] -bxor 1
        [IO.File]::WriteAllBytes($file, $bytes)
        Expect-Rejection { & (Join-Path $scripts 'Test-DlssgSm86Package.ps1') -PackageDirectory $package }
        Restore-Package
    }
    foreach ($name in @('nvngx_dlssg.dll','version.dll','winmm.dll','dbghelp.dll','dinput8.dll','dlssg_sm86.ini')) {
        $file = Join-Path $package $name
        [IO.File]::WriteAllText($file, 'legacy')
        Expect-Rejection { & (Join-Path $scripts 'Test-DlssgSm86Package.ps1') -PackageDirectory $package }
        Remove-Item -LiteralPath $file
    }
    # An invalid dependency must not touch an existing package.
    foreach ($record in $contract.Files) {
        $target = Join-Path $dependency $record.Name
        New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $record.Name) -Destination $target
    }
    [IO.File]::WriteAllText((Join-Path $dependency 'native.ini'), 'damaged')
    Expect-Rejection { & (Join-Path $scripts 'Copy-DlssgSm86Runtime.ps1') -RuntimeDirectory $dependency -OutputDirectory $package }
    $null = Assert-DlssgSm86Files $runtime
    $checks++
    # Unknown root DLLs must survive and force package rejection.
    [IO.File]::WriteAllText((Join-Path $package 'nvngx_dlssg.dll'), 'not an owned legacy binary')
    [IO.File]::WriteAllText((Join-Path $package 'nvngx_dlss.dll'), 'SR untouched')
    Expect-Rejection { Restore-Package }
    if ([IO.File]::ReadAllText((Join-Path $package 'nvngx_dlssg.dll')) -ne 'not an owned legacy binary' -or
        [IO.File]::ReadAllText((Join-Path $package 'nvngx_dlss.dll')) -ne 'SR untouched') { throw 'Incorrect copy cleanup.' }
    $checks++
    Write-Output "DLSSG packaging: $checks checks passed; no binary was loaded or executed."
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (!$resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'Magpie DLSSG 测试 *') { throw 'Unsafe fixture cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
