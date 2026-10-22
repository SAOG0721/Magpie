param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'DlssgSm86Package.ps1')
$contract = Assert-DlssgSm86Files $RuntimeDirectory
$destination = Join-Path $OutputDirectory 'runtimes/dlssg_sm86'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($record in $contract.Files) {
    $target = Join-Path $destination $record.Name
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $record.Name) -Destination $target -Force
}
# Never remove an arbitrary root DLL by name. Only a pinned prior component is
# moved aside, retaining recovery information and all unrelated runtimes.
$legacy = Join-Path $OutputDirectory 'nvngx_dlssg.dll'
$old = [pscustomobject]@{ Bytes=7460976; SHA256='ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82' }
if (Test-DlssgSm86File $legacy $old) {
    $backup = Join-Path $OutputDirectory ('component-backups/dlssg-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    Move-Item -LiteralPath $legacy -Destination $backup
}
& (Join-Path $PSScriptRoot 'Test-DlssgSm86Package.ps1') -PackageDirectory $OutputDirectory
