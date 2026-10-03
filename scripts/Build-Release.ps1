param(
    [string]$PackageName = "Magpie-Experimental-x64",
    [string]$ReleaseDirectory,
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$Version,
    [ValidateRange(1, 64)]
    [int]$MaxCpuCount = 2,
    [ValidateRange(1, 64)]
    [int]$MaxCompilerProcesses = 1,
    [switch]$AllowDirtySource,
    [switch]$IncludeSymbols,
    [switch]$SkipBuild,
    [switch]$RequireMagpieClosed,
    [switch]$EnableFrameTrace,
    [switch]$EnableAmdLmxxfNR,
    [string]$AmdLmxxfNRRuntimeDir,
    [switch]$EnableFSR3ZeroMV,
    [string]$FSR3SdkDir,
    [switch]$EnableAmdOpticalFlow,
    [switch]$EnableXeSSFrameGeneration,
    [string]$XeSSSdkDir,
    [switch]$EnableNativeBackendTiming
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$sourceRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$workspaceRoot = Split-Path $sourceRoot -Parent
$releaseRoot = [System.IO.Path]::GetFullPath((Join-Path $workspaceRoot "release"))

function Find-MSBuild {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        $amd64Command = Join-Path (Split-Path $command.Source -Parent) "amd64\MSBuild.exe"
        if (Test-Path -LiteralPath $amd64Command) { return $amd64Command }
        return $command.Source
    }

    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($vswhere in $vswhereCandidates) {
        $installationPath = & $vswhere -latest -products * `
            -requires Microsoft.Component.MSBuild -property installationPath |
            Select-Object -First 1
        if ($installationPath) {
            $amd64MsBuild = Join-Path $installationPath "MSBuild\Current\Bin\amd64\MSBuild.exe"
            if (Test-Path -LiteralPath $amd64MsBuild) { return $amd64MsBuild }
        }

        $result = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($result -and (Test-Path -LiteralPath $result)) { return $result }
    }
    throw "MSBuild was not found. Install the Visual Studio C++ build tools."
}

function Add-ConanToPath {
    if (Get-Command conan -ErrorAction SilentlyContinue) { return }

    $roots = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Python"),
        (Join-Path $env:APPDATA "Python")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($root in $roots) {
        $conan = Get-ChildItem -LiteralPath $root -Filter conan.exe -File -Recurse `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($conan) {
            $env:Path = "$($conan.DirectoryName);$env:Path"
            return
        }
    }
    throw "Conan was not found. Install Conan 2 and make conan.exe available on PATH."
}

function Add-CMakeToPath {
    if (Get-Command cmake.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installations = & $vswhere -products * -property installationPath
        foreach ($installation in $installations) {
            $cmakeDir = Join-Path $installation "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
            if (Test-Path -LiteralPath (Join-Path $cmakeDir "cmake.exe")) {
                $env:Path = "$cmakeDir;$env:Path"
                return
            }
        }
    }
    throw "CMake was not found. Install the Visual Studio CMake component or add cmake.exe to PATH."
}

function Find-Git {
    $command = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $candidates = @((Join-Path $env:ProgramFiles "Git\cmd\git.exe"))
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        foreach ($installation in (& $vswhere -products * -property installationPath)) {
            $candidates += Join-Path $installation `
                "Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd\git.exe"
        }
    }
    return $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}

$gitPath = Find-Git
function Invoke-Git([string[]]$Arguments) {
    if (!$gitPath) { return $null }
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $value = & $gitPath -c "safe.directory=$sourceRoot" -C $sourceRoot @Arguments 2>$null
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($exitCode -ne 0) { return $null }
    return ($value | Out-String).Trim()
}

$commit = Invoke-Git @("rev-parse", "HEAD")
$shortCommit = Invoke-Git @("rev-parse", "--short=12", "HEAD")
$commitTimeText = Invoke-Git @("show", "-s", "--format=%cI", "HEAD")
$exactTag = Invoke-Git @("describe", "--tags", "--exact-match", "HEAD")
$sourceDirty = [bool](Invoke-Git @("status", "--porcelain=v1", "--untracked-files=all"))
if ($sourceDirty -and !$AllowDirtySource) {
    throw "The source tree has uncommitted changes. Commit them first, or use -AllowDirtySource for a local test package."
}

if (!$Version) {
    if ($exactTag) {
        $Version = $exactTag -replace '^v', ''
    } elseif ($shortCommit) {
        $Version = "0.0.0-dev+$shortCommit"
    } else {
        $Version = "0.0.0-dev"
    }
}

$versionMatch = [regex]::Match($Version, '^(?:v)?(\d+)\.(\d+)\.(\d+)')
if (!$versionMatch.Success) {
    throw "Version must begin with major.minor.patch: $Version"
}

if (!$ReleaseDirectory) {
    $ReleaseDirectory = if ($Version.StartsWith('v', [System.StringComparison]::OrdinalIgnoreCase)) {
        $Version
    } else {
        "v$Version"
    }
}
$releaseContainer = [System.IO.Path]::GetFullPath((Join-Path $releaseRoot $ReleaseDirectory))
$stagingDir = [System.IO.Path]::GetFullPath((Join-Path $releaseContainer $PackageName))
$buildOutput = $stagingDir
$intermediateRoot = Join-Path $releaseContainer "obj/$Platform/$Configuration"
if ($releaseContainer.TrimEnd('\', '/') -in @(
    (Join-Path $releaseRoot 'v0.6.7-local'), (Join-Path $releaseRoot 'v0.6.8-local'))) {
    throw "Use the matching Deploy-Local script to preserve the local runtime and user settings."
}
$symbolsDir = [System.IO.Path]::GetFullPath((Join-Path $releaseContainer "$PackageName-symbols"))
$zipPath = [System.IO.Path]::GetFullPath((Join-Path $releaseContainer "$PackageName.zip"))

if (!$releaseContainer.StartsWith($releaseRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Release container escaped the release directory."
}
if (!$stagingDir.StartsWith($releaseContainer + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase) -or
    !$symbolsDir.StartsWith($releaseContainer + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase) -or
    !$zipPath.StartsWith($releaseContainer + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Release path escaped the release directory."
}

function Stop-RunningMagpie {
    $running = Get-Process -Name "Magpie" -ErrorAction SilentlyContinue
    if (!$running) { return }

    try {
        $running | Stop-Process -Force -ErrorAction Stop
    } catch {
        $processIds = ($running.Id | ForEach-Object { [string][int]$_ }) -join ','
        $command = "Stop-Process -Id $processIds -Force"
        $elevated = Start-Process -FilePath "powershell.exe" -Verb RunAs -WindowStyle Hidden `
            -ArgumentList @("-NoProfile", "-Command", $command) -Wait -PassThru
        if ($elevated.ExitCode -ne 0) {
            throw "Elevated Magpie shutdown failed with exit code $($elevated.ExitCode)."
        }
    }

    Start-Sleep -Milliseconds 250
    if (Get-Process -Name "Magpie" -ErrorAction SilentlyContinue) {
        throw "Magpie is still running after the shutdown attempt."
    }
}

# Magpie and its PRI must come from the same complete build. Stop the running
# application before Rebuild so MSBuild never falls back to deploying a lone
# replacement executable around a locked output directory.
if ($RequireMagpieClosed) {
    # Only the package being rebuilt must be closed. A different local release
    # may remain running while a new distribution is prepared in its own folder.
    $targetExecutable = Join-Path $buildOutput 'Magpie.exe'
    if (Test-Path -LiteralPath $targetExecutable) {
        try {
            $probe = [IO.File]::Open($targetExecutable, [IO.FileMode]::Open,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $probe.Dispose()
        } catch {
            throw "Close the Magpie instance using $targetExecutable before rebuilding; no process was terminated."
        }
    }
} else {
    Stop-RunningMagpie
}

if (!$SkipBuild) {
	# MSBuild Rebuild removes known project outputs but leaves arbitrary runtime
	# files copied by older feature sets. Start from an empty configuration
	# directory so removed optional components cannot leak into a new package.
	$buildRoot = $releaseContainer
	if (!$buildOutput.StartsWith(
		$buildRoot + [System.IO.Path]::DirectorySeparatorChar,
		[System.StringComparison]::OrdinalIgnoreCase)) {
		throw "Build output escaped the release version directory: $buildOutput"
	}
	if (Test-Path -LiteralPath $buildOutput) {
		Get-ChildItem -LiteralPath $buildOutput -Force |
			Remove-Item -Recurse -Force
	}

    $msbuild = Find-MSBuild
    Add-ConanToPath
    Add-CMakeToPath

    $disablePdb = if ($IncludeSymbols) { "false" } else { "true" }
    $multiToolTask = if ($MaxCompilerProcesses -gt 1) { "true" } else { "false" }
    $msbuildArgs = @(
        "Magpie.slnx", "/m:$MaxCpuCount", "/nr:false", "/v:minimal", "/t:Rebuild", "/restore", "/p:RestorePackagesConfig=true",
        "/p:Configuration=$Configuration", "/p:Platform=$Platform",
        "/p:MajorVersion=$($versionMatch.Groups[1].Value)",
        "/p:MinorVersion=$($versionMatch.Groups[2].Value)",
        "/p:PatchVersion=$($versionMatch.Groups[3].Value)",
        "/p:VersionString=$Version", "/p:CommitId=$shortCommit",
        "/p:PreferredToolArchitecture=x64",
        "/p:UseMultiToolTask=$multiToolTask", "/p:CL_MPCount=$MaxCompilerProcesses",
        "/p:MultiProcMaxCount=$MaxCompilerProcesses", "/p:EnforceProcessCountAcrossBuilds=true",
        "/p:DisablePDB=$disablePdb", "/p:ReproducibleBuild=true",
        ('/p:OutDir=' + $buildOutput.Replace('\', '/') + '/'),
        ('/p:MagpieIntermediateRoot=' + $intermediateRoot.Replace('\', '/') + '/')
    )
    if ($EnableAmdLmxxfNR) {
        if (!$AmdLmxxfNRRuntimeDir) { throw "Specify -AmdLmxxfNRRuntimeDir for the AMD NR build." }
        $msbuildArgs += @("/p:EnableAmdLmxxfNR=true", "/p:AmdLmxxfNRRuntimeDir=$([System.IO.Path]::GetFullPath($AmdLmxxfNRRuntimeDir))")
    }
    if ($EnableFSR3ZeroMV -or $EnableAmdOpticalFlow) {
        if (!$FSR3SdkDir) { throw "Specify -FSR3SdkDir for FSR / AMD optical flow." }
        $msbuildArgs += "/p:FSR3SdkDir=$([System.IO.Path]::GetFullPath($FSR3SdkDir))"
    }
    if ($EnableFSR3ZeroMV) { $msbuildArgs += "/p:EnableFSR3ZeroMV=true" }
    if ($EnableAmdOpticalFlow) { $msbuildArgs += "/p:EnableAmdOpticalFlow=true" }
    elseif ($EnableFSR3ZeroMV) { $msbuildArgs += "/p:EnableAmdOpticalFlow=false" }
    if ($EnableXeSSFrameGeneration) {
        if (!$XeSSSdkDir) { throw "Specify -XeSSSdkDir for XeSS FG." }
        $msbuildArgs += @("/p:EnableXeSSFrameGeneration=true", "/p:XeSSSdkDir=$([System.IO.Path]::GetFullPath($XeSSSdkDir))")
    }
    if ($EnableNativeBackendTiming) { $msbuildArgs += "/p:EnableNativeBackendTiming=true" }
    $traceEnabled = if ($EnableFrameTrace) { "true" } else { "false" }
    $msbuildArgs += "/p:EnableFrameTrace=$traceEnabled"
    if ($IncludeSymbols) {
        $msbuildArgs += @(
            "/p:GenerateReleaseSymbols=true"
        )
    }

    Push-Location $sourceRoot
    try {
        & $msbuild @msbuildArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Release build failed with exit code $LASTEXITCODE."
        }
    } finally {
        Pop-Location
    }
}

$requiredRuntimePaths = @(
    "Magpie.exe",
    "resources.pri",
    "Microsoft.UI.Xaml.dll",
    "TouchHelper.exe",
    "Updater.exe",
    "effects"
)
$missingRuntimePaths = @($requiredRuntimePaths | Where-Object {
    !(Test-Path -LiteralPath (Join-Path $buildOutput $_))
})
if ($missingRuntimePaths.Count -ne 0) {
    throw "Release runtime layout is incomplete in $buildOutput. Missing: $($missingRuntimePaths -join ', ')"
}

if ($IncludeSymbols) {
    $requiredSymbolPaths = @("Magpie.pdb", "Magpie.map")
    $missingSymbolPaths = @($requiredSymbolPaths | Where-Object {
        !(Test-Path -LiteralPath (Join-Path $buildOutput $_))
    })
    if ($missingSymbolPaths.Count -ne 0) {
        throw "Required Magpie symbols were not generated in $buildOutput. Missing: $($missingSymbolPaths -join ', ')"
    }

    $symbolFiles = @(Get-ChildItem -LiteralPath $buildOutput -File -Recurse | Where-Object {
        $_.Extension -in ".pdb", ".map"
    })

    if (Test-Path -LiteralPath $symbolsDir) {
        Get-ChildItem -LiteralPath $symbolsDir -Force |
            Remove-Item -Recurse -Force
    } else {
        New-Item -ItemType Directory -Path $symbolsDir | Out-Null
    }

    foreach ($symbolFile in $symbolFiles) {
        $relativePath = $symbolFile.FullName.Substring($buildOutput.Length).TrimStart('\', '/')
        $destination = Join-Path $symbolsDir $relativePath
        $destinationParent = Split-Path $destination -Parent
        New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
        Copy-Item -LiteralPath $symbolFile.FullName -Destination $destination
    }
}

# The complete build is already in the package directory. Symbols were copied
# above; remove only linker/debug artifacts from the distributable package.
Get-ChildItem -LiteralPath $stagingDir -File | Where-Object {
    $_.Extension -in ".pdb", ".map", ".lib", ".exp" -or $_.Name -eq "Magpie.next.exe"
} | Remove-Item -Force

# Never package per-user runtime state left by local test launches.
foreach ($runtimeStateName in @("cache", "logs", "config", "PortableAppData", "config.json")) {
    $runtimeStatePath = Join-Path $stagingDir $runtimeStateName
    if (Test-Path -LiteralPath $runtimeStatePath) {
        Remove-Item -LiteralPath $runtimeStatePath -Recurse -Force
    }
}

Copy-Item -LiteralPath (Join-Path $sourceRoot "LICENSE") `
    -Destination (Join-Path $stagingDir "LICENSE-Magpie.txt")
Copy-Item -LiteralPath (Join-Path $sourceRoot "docs\README-EXPERIMENTAL-RELEASE.txt") `
    -Destination (Join-Path $stagingDir "README-Experimental.txt")
$packagedReadmePath = Join-Path $stagingDir "README-Experimental.txt"
$packagedReadme = Get-Content -LiteralPath $packagedReadmePath -Raw -Encoding UTF8
$packagedReadme = $packagedReadme -replace '^Magpie Experimental v[^ ]+ x64', "Magpie Experimental v$Version x64"
Set-Content -LiteralPath $packagedReadmePath -Value $packagedReadme -Encoding UTF8 -NoNewline
Copy-Item -LiteralPath (Join-Path $sourceRoot "docs\THIRD_PARTY_AND_REDISTRIBUTION.md") `
    -Destination (Join-Path $stagingDir "THIRD-PARTY-NOTICES.md")

$versionNotes = Join-Path $sourceRoot "docs\RELEASE_NOTES_v$Version.md"
if (!(Test-Path -LiteralPath $versionNotes)) {
    $versionNotes = Join-Path $sourceRoot "docs\RELEASE_NOTES_v$Version-experimental.md"
}
if (Test-Path -LiteralPath $versionNotes) {
    Copy-Item -LiteralPath $versionNotes -Destination (Join-Path $stagingDir "RELEASE-NOTES.md")
}
$frameSyncGuide = Join-Path $sourceRoot "docs\FRAME_SYNC_GUIDE.md"
if (Test-Path -LiteralPath $frameSyncGuide) {
    Copy-Item -LiteralPath $frameSyncGuide -Destination (Join-Path $stagingDir "FRAME_SYNC_GUIDE.md")
}
$parameterInputGuide = Join-Path $sourceRoot "docs\experimental\testing\PARAMETER-INPUT.md"
if (Test-Path -LiteralPath $parameterInputGuide) {
    Copy-Item -LiteralPath $parameterInputGuide -Destination (Join-Path $stagingDir "PARAMETER-INPUT.md")
}

foreach ($guide in @(
    @{ source = 'docs/experimental/effects/DLSSNR-MULTIPASS.md'; target = 'DLSSNR-MULTIPASS.md' },
    @{ source = 'docs/experimental/effects/XESSFG.md'; target = 'XESSFG.md' },
    @{ source = 'docs/experimental/design/XESSFG-COMPATIBILITY-NOTICE.md'; target = 'XESSFG-COMPATIBILITY-NOTICE.md' }
)) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot $guide.source) -Destination (Join-Path $stagingDir $guide.target)
}

$featureOptions = [ordered]@{}
$buildOptionsPath = Join-Path $sourceRoot "src\BuildOptions.props"
[xml]$buildOptions = Get-Content -LiteralPath $buildOptionsPath
$supportedFeatureOptions = @{}
$buildOptions.Project.PropertyGroup.ChildNodes | Where-Object {
    $_.Name -like "Enable*"
} | ForEach-Object {
    $supportedFeatureOptions[$_.Name] = $true
    if ($_.InnerText -in @("true", "false")) {
        $featureOptions[$_.Name] = [string]$_.InnerText
    }
}
$userOptionsPath = Join-Path $sourceRoot "src\BuildOptions.props.user"
if (Test-Path -LiteralPath $userOptionsPath) {
    [xml]$userOptions = Get-Content -LiteralPath $userOptionsPath
    $properties = $userOptions.Project.PropertyGroup.ChildNodes | Where-Object {
        $_.Name -like "Enable*" -and $supportedFeatureOptions.ContainsKey($_.Name)
    }
    foreach ($property in $properties) {
        $featureOptions[$property.Name] = [string]$property.InnerText
    }
}
# Match the compatibility defaults as well as explicit user overrides.
if (!$featureOptions.Contains("EnableDLSSSR") -and
    $featureOptions.Contains("EnableDLSSZeroMV")) {
    $featureOptions["EnableDLSSSR"] = $featureOptions["EnableDLSSZeroMV"]
}
if (!$featureOptions.Contains("EnableAmdOpticalFlow") -and
    $featureOptions.Contains("EnableFSR3ZeroMV")) {
    $featureOptions["EnableAmdOpticalFlow"] =
        $featureOptions["EnableFSR3ZeroMV"]
}

$featureOptions["EnableFrameTrace"] = if ($EnableFrameTrace) { "true" } else { "false" }
foreach ($entry in @(@('EnableAmdLmxxfNR',[bool]$EnableAmdLmxxfNR),
    @('EnableFSR3ZeroMV',[bool]$EnableFSR3ZeroMV), @('EnableXeSSFrameGeneration',[bool]$EnableXeSSFrameGeneration),
    @('EnableNativeBackendTiming',[bool]$EnableNativeBackendTiming))) {
    if ($entry[1]) { $featureOptions[$entry[0]]='true' }
}
if ($EnableAmdOpticalFlow) { $featureOptions['EnableAmdOpticalFlow']='true' }
elseif ($EnableFSR3ZeroMV) { $featureOptions['EnableAmdOpticalFlow']='false' }
if ($featureOptions["EnableFrameTrace"] -eq "true") {
    $diagnosticsDir = Join-Path $stagingDir "Diagnostics"
    New-Item -ItemType Directory -Path $diagnosticsDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot "scripts\Analyze-FrameTrace.py") -Destination $diagnosticsDir
    Copy-Item -LiteralPath (Join-Path $sourceRoot "docs\FRAME_TRACE_GUIDE.md") -Destination (Join-Path $diagnosticsDir "README.md")
}

$fileRecords = Get-ChildItem -LiteralPath $stagingDir -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relativePath = $_.FullName.Substring($stagingDir.Length).TrimStart('\', '/')
    [ordered]@{
        path = $relativePath.Replace('\', '/')
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        fileVersion = $_.VersionInfo.FileVersion
        productVersion = $_.VersionInfo.ProductVersion
    }
}

$sourceDate = if ($commitTimeText) {
    [DateTimeOffset]::Parse($commitTimeText).UtcDateTime
} else {
    [DateTime]::SpecifyKind([DateTime]"1970-01-01", [DateTimeKind]::Utc)
}
$manifest = [ordered]@{
    schemaVersion = 1
    package = $PackageName
    version = $Version
    commit = $commit
    sourceDirty = $sourceDirty
    sourceDateUtc = $sourceDate.ToString("o")
    configuration = $Configuration
    platform = $Platform
    featureOptions = $featureOptions
    files = @($fileRecords)
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content `
    -LiteralPath (Join-Path $stagingDir "build-manifest.json") -Encoding utf8

# Normalize staging timestamps to the source commit time. Combined with the
# manifest this makes repeated packages comparable and avoids local wall-clock
# metadata in the archive.
Get-ChildItem -LiteralPath $stagingDir -Recurse -Force | ForEach-Object {
    $item = $_
    $normalized = $false
    for ($attempt = 1; $attempt -le 5; ++$attempt) {
        try {
            $item.LastWriteTimeUtc = $sourceDate
            $normalized = $true
            break
        } catch {
            if ($attempt -lt 5) { Start-Sleep -Milliseconds 200 }
        }
    }
    if (!$normalized) {
        Write-Warning "Could not normalize timestamp for $($item.FullName); ZIP entry time remains normalized."
    }
}
# The staging root can remain briefly open in Explorer or by a recently exited
# GUI process. ZIP entries are created from its children, so the root timestamp
# does not affect archive reproducibility.
try {
    (Get-Item -LiteralPath $stagingDir).LastWriteTimeUtc = $sourceDate
} catch [System.IO.IOException] {
    Write-Warning "Could not normalize the staging root timestamp: $($_.Exception.Message)"
}

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open(
    $zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -LiteralPath $stagingDir -File -Recurse | Sort-Object FullName | ForEach-Object {
        $relativePath = $_.FullName.Substring($stagingDir.Length).TrimStart('\', '/').Replace('\', '/')
        $entryName = "$PackageName/$relativePath"
        $entry = $archive.CreateEntry(
            $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]$sourceDate

        $inputStream = [System.IO.File]::OpenRead($_.FullName)
        $outputStream = $entry.Open()
        try {
            $inputStream.CopyTo($outputStream)
        } finally {
            $outputStream.Dispose()
            $inputStream.Dispose()
        }
    }
} finally {
    $archive.Dispose()
}

$zip = Get-Item -LiteralPath $zipPath
$hash = Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
Write-Host "Version:           $Version"
Write-Host "Commit:            $commit"
Write-Host "Release directory: $stagingDir"
if ($IncludeSymbols) {
    Write-Host "Release symbols:   $symbolsDir"
}
Write-Host "Release ZIP:       $zipPath"
Write-Host "ZIP bytes:         $($zip.Length)"
Write-Host "SHA256:            $($hash.Hash)"
