param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'DlssgSm86Package.ps1')
$null = Assert-DlssgSm86Files (Join-Path $PackageDirectory 'runtimes/dlssg_sm86')
# Root utility proxies can execute before our LUID-based preparation.
# Discovery uses the pinned proxy's adjacent runtime: Prepare explicitly loads
# it, validates the returned bundle and registers that directory with NGX. A
# root FG DLL is not an entry point and can mask failure of the managed route.
foreach ($name in @('nvngx_dlssg.dll', 'version.dll', 'winmm.dll', 'dbghelp.dll', 'dinput8.dll', 'dlssg_sm86.ini')) {
    if (Test-Path -LiteralPath (Join-Path $PackageDirectory $name)) {
        throw "Unexpected root DLSSG/proxy file: $name. Package may bypass the managed loader."
    }
}
Write-Output 'DLSSG package identities verified for managed bundle discovery. This check does not load the proxy or verify NGX discovery, Feature creation or GPU execution.'
