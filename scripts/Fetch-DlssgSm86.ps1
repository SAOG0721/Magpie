param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../dependencies/dlssg-sm86'),
    [string]$ArchivePath
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'DlssgSm86Package.ps1')
$contract = Get-DlssgSm86Contract
# Retain the exact archive and previous dependency for recovery. Never edit the
# shared runtime cache or a loaded product directory in this development fetch.
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($OutputDirectory.TrimEnd('\','/') -eq [IO.Path]::GetPathRoot($OutputDirectory).TrimEnd('\','/')) {
    throw 'A filesystem root cannot be used as a runtime dependency directory.'
}
$staging = $OutputDirectory + '.staging.' + [guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $staging -Force | Out-Null
if (!$ArchivePath) {
    $ArchivePath = $staging + '.zip'
    Invoke-WebRequest -Uri $contract.ArchiveURL -OutFile $ArchivePath
}
$archiveRecord = [pscustomobject]@{ Bytes=$contract.ArchiveSize; SHA256=$contract.ArchiveSHA256 }
if (!(Test-DlssgSm86File $ArchivePath $archiveRecord)) { throw 'DLSSG release archive identity mismatch.' }
# This exact hash pins the upstream archive contents before extraction.
$extracted = $staging + '.upstream'
Expand-Archive -LiteralPath $ArchivePath -DestinationPath $extracted
foreach ($record in $contract.Files) {
    $source = if ($record.Name.EndsWith('.ini')) {
        Join-Path $PSScriptRoot "../src/DlssgSm86/$($record.Name)"
    } else { Join-Path $extracted $record.Name }
    $target = Join-Path $staging $record.Name
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}
$null = Assert-DlssgSm86Files $staging
$backup = $OutputDirectory + '.backup.' + [guid]::NewGuid().ToString('N')
# All moves remain siblings of the resolved, explicitly selected dependency.
if ((Split-Path $backup) -ne (Split-Path $OutputDirectory) -or
    (Split-Path $staging) -ne (Split-Path $OutputDirectory)) { throw 'Unsafe dependency move paths.' }
if (Test-Path -LiteralPath $OutputDirectory) {
    Move-Item -LiteralPath $OutputDirectory -Destination $backup
}
try { Move-Item -LiteralPath $staging -Destination $OutputDirectory }
catch {
    if ((Test-Path -LiteralPath $backup) -and !(Test-Path -LiteralPath $OutputDirectory)) {
        Move-Item -LiteralPath $backup -Destination $OutputDirectory
    }
    throw
}
Write-Output "Verified DLSSG 0.4.0 dependency: $OutputDirectory; archive/extraction retained, no binary executed."
