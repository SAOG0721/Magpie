#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [string]$OptionsDirectory
)

$ErrorActionPreference = 'Stop'
# A release uses the same prepared dependency as an ordinary build. Keep the
# optional archive beside the main ZIP, never inside the application directory.
$files = @(@{ Name = 'nvngx_dlssnr.dll'; Directory = $PackageDirectory })
if ($OptionsDirectory) {
    $files += @{ Name = 'DLSSNR-DLL-Options.zip'; Directory = $OptionsDirectory }
}

# Check every source before replacing any destination. The runtime directory
# remains overridable for developer builds; hashes verify copy integrity.
foreach ($file in $files) {
    $file.Source = [IO.Path]::GetFullPath((Join-Path $RuntimeDirectory $file.Name))
    if (!(Test-Path -LiteralPath $file.Source -PathType Leaf)) {
        throw "Missing DLSSNR distribution input: $($file.Source). See docs/experimental/RELEASE-WORKFLOW.md."
    }
    $file.Hash = (Get-FileHash -LiteralPath $file.Source -Algorithm SHA256).Hash
}
foreach ($file in $files) {
    New-Item -ItemType Directory -Path $file.Directory -Force | Out-Null
    $destination = [IO.Path]::GetFullPath((Join-Path $file.Directory $file.Name))
    if ($destination -ine $file.Source) {
        Copy-Item -LiteralPath $file.Source -Destination $destination -Force
    }
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $file.Hash) {
        throw "DLSSNR distribution copy mismatch: $destination"
    }
}
Write-Host "DLSSNR distribution inputs copied and verified."
