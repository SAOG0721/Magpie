#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
& (Join-Path $repoRoot 'scripts/Generate-EffectCatalog.ps1') -Check
$review = & (Join-Path $repoRoot 'scripts/Read-EffectCatalogReview.ps1')
function Get-CanonicalHash([string]$Path) {
    $bytes = [Text.Encoding]::UTF8.GetBytes([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($Path)).Replace("`r`n", "`n"))
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
}
if ($review.effects.Count -ne 166 -or $review.families.Count -ne 13 -or $review.subfamilies.Count -ne 2) { throw 'Current review coverage differs.' }
foreach ($entry in $review.effects) {
    if ($entry.review.status -ne 'source-reviewed') { throw "Unreviewed effect: $($entry.id)" }
    $sourcePath = Join-Path $repoRoot $entry.source.path
    if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash -ine $entry.source.sha256) {
        # Git may check out reviewed LF sources as CRLF on Windows.
        $canonical = [Text.Encoding]::UTF8.GetBytes([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($sourcePath)).Replace("`r`n", "`n"))
        $canonicalHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($canonical))
        if ($canonicalHash -ine $entry.source.sha256) { throw "Source changed since review: $($entry.id)" }
    }
    foreach ($path in $entry.review.runtimeEvidence) {
        if (!(Test-Path -LiteralPath (Join-Path $repoRoot $path) -PathType Leaf)) { throw "Missing review evidence: $path" }
    }
    # Native markers alone cannot establish backend behaviour. When recorded,
    # require a complete hash set, normalized for Git's Windows line endings.
    $hashes = $entry.review.runtimeEvidenceSha256
    if ($entry.id -match '^(NvidiaVFX|RTXVideo|FSR3FG)\\' -and !$hashes) { throw "Missing native review hashes: $($entry.id)" }
    if ($hashes) {
        $paths = @($hashes.PSObject.Properties.Name)
        if (!$paths.Count -or @(Compare-Object @($entry.review.runtimeEvidence) $paths).Count) { throw "Incomplete backend evidence: $($entry.id)" }
        foreach ($path in $paths) {
            if ((Get-CanonicalHash (Join-Path $repoRoot $path)) -ine $hashes.$path) { throw "Backend changed since description review: $($entry.id) / $path" }
        }
    }
    foreach ($language in @('zh-Hans', 'en-US')) {
        if ($entry.$language.summary.Contains("`n") -or $entry.$language.summary.Length -gt 90) { throw "Summary is too long: $language $($entry.id)" }
    }
}
$catalog = Get-Content -LiteralPath (Join-Path $repoRoot 'src/Magpie/EffectCatalog/zh-Hans.json') -Raw | ConvertFrom-Json
$englishCatalogText = Get-Content -LiteralPath (Join-Path $repoRoot 'src/Magpie/EffectCatalog/en-US.json') -Raw
if ($englishCatalogText -match '[\u3400-\u9fff]') { throw 'Chinese text remains in the English catalog.' }
$englishCatalog = $englishCatalogText | ConvertFrom-Json
$effectRoot = Join-Path $repoRoot 'src/Effects'
$ids = @(Get-ChildItem -LiteralPath $effectRoot -Filter '*.hlsl' -Recurse -File | ForEach-Object { [IO.Path]::GetRelativePath($effectRoot, $_.FullName).Replace('.hlsl', '') })
if (@(Compare-Object $ids @($catalog.effects.id)).Count -or @($catalog.effects.id | Group-Object | Where-Object Count -gt 1).Count) { throw 'Catalog and installed IDs differ or contain duplicates.' }
if (@(Compare-Object @($catalog.effects.id) @($englishCatalog.effects.id) -SyncWindow 0).Count) { throw 'English catalog effect IDs or ordering differ.' }
if (@(Compare-Object @($catalog.categories.id) @($englishCatalog.categories.id) -SyncWindow 0).Count) { throw 'English catalog category IDs or ordering differ.' }
for ($i = 0; $i -lt $catalog.categories.Count; ++$i) {
    if (@($catalog.categories[$i].subcategories).Count -ne @($englishCatalog.categories[$i].subcategories).Count) { throw 'English catalog subcategory structure differs.' }
}
for ($i = 0; $i -lt $catalog.effects.Count; ++$i) {
    $source = $catalog.effects[$i]
    $translated = $englishCatalog.effects[$i]
    foreach ($field in @('id','category','level')) {
        if ($source.$field -ne $translated.$field) { throw "English catalog changed structural field $field for $($source.id)" }
    }
    if (($source.purposes -join '|') -ne ($translated.purposes -join '|') -or
        $source.family.id -ne $translated.family.id -or $source.subfamily.id -ne $translated.subfamily.id) {
        throw "English catalog changed classification metadata for $($source.id)"
    }
    foreach ($field in @('name','summary','details','category','search')) {
        if (!$translated.$field) { throw "Missing English $field for $($source.id)" }
        if ($translated.$field -match '[\u3400-\u9fff]') { throw "Chinese text remains in English $field for $($source.id)" }
    }
    # Subcategory names are localized; they must refer to the same slot, not
    # merely to any valid category label in each language.
    $zhCategory = $catalog.categories | Where-Object id -eq $source.category
    $enCategory = $englishCatalog.categories | Where-Object id -eq $translated.category
    if (@($zhCategory.subcategories).Count) {
        $zhIndex = [Array]::IndexOf(@($zhCategory.subcategories.name), $source.subcategory)
        $enIndex = [Array]::IndexOf(@($enCategory.subcategories.name), $translated.subcategory)
        if ($zhIndex -lt 0 -or $enIndex -ne $zhIndex) { throw "Missing or mismatched bilingual subcategory: $($source.id)" }
    } elseif ($source.subcategory -or $translated.subcategory) { throw "Unexpected subcategory: $($source.id)" }
}
foreach ($entry in $catalog.effects) {
    foreach ($field in @('name','summary','details','category','search')) { if (!$entry.$field) { throw "Missing $field for $($entry.id)" } }
    if ($entry.category -notin $catalog.categories.id) { throw 'Unknown category' }
    $category = $catalog.categories | Where-Object id -eq $entry.category
    if ($entry.subcategory -and $entry.subcategory -notin $category.subcategories.name) { throw "Unknown subcategory: $($entry.id)" }
    foreach ($family in @($entry.family, $entry.subfamily)) {
        if ($family -and (!$family.id -or !$family.name -or !$family.summary)) { throw "Incomplete family: $($entry.id)" }
    }
    if ($entry.subfamily -and !$entry.family) { throw 'Subfamily without a parent' }
}
$dlssnr = $catalog.effects | Where-Object id -eq 'DLSSNR\DLSSNR_AI_Filter'
if ($dlssnr.category -ne 'style' -or $dlssnr.purposes -contains 'cleanup' -or $dlssnr.summary -match '降噪') { throw 'DLSSNR purpose regression' }
if ($dlssnr.details -notmatch '1～3' -or $dlssnr.details -notmatch '调整输入分辨率' -or $dlssnr.details -match '615|RTX 50|手动替换') { throw 'DLSSNR capability or compatibility guidance regression' }
# Detail-control application timing lives in parameter help; checked by
# validate_effect_descriptions.py together with the bilingual resources.
$limiter = $catalog.effects | Where-Object id -eq 'FrameRate_Filter'
if ($limiter.details -notmatch '帧率与刷新' -or $limiter.details -notmatch '基础帧率') { throw 'Unified frame-rate guidance regression' }
$dlss = $catalog.effects | Where-Object id -eq 'DLSS\DLSS_SR'
if ($dlss.category -ne 'upscale' -or $dlss.subcategory -ne '时序重建' -or $dlss.name -ne 'DLSS SR' -or
    $dlss.purposes -contains 'antialiasing') { throw 'DLSS SR classification regression' }
foreach ($id in @('FSR2\FSR2_SR', 'FSR3\FSR3_SR', 'FSR4\FSR4_SR', 'XeSS\XeSS_SR')) {
    $peer = $catalog.effects | Where-Object id -eq $id
    if ($peer.category -ne $dlss.category -or $peer.subcategory -ne $dlss.subcategory) { throw 'Temporal SR entries are in different categories' }
}
$rtx = @($catalog.effects | Where-Object id -in @('RTXVideo\RTXVideo_Denoise','RTXVideo\RTXVideo_VSR'))
if ($rtx.Count -ne 2 -or @($rtx.name | Sort-Object -Unique).Count -ne 2) { throw 'RTX Video grouping regression' }
foreach ($entry in $rtx) {
    if ($entry.level -ne 'situational' -or ($entry.id -eq 'RTXVideo\RTXVideo_Denoise' -and $entry.details -notmatch '实时切换')) { throw 'RTX Video maturity or live-strength regression' }
    $file = Join-Path $effectRoot ($entry.id + '.hlsl')
    if ((Get-Content -LiteralPath $file -Raw) -notmatch '(?s)//!DEFAULT 1.*//!OPTION 0 Low.*//!OPTION 3 Ultra.*int strength;') { throw 'RTX strength metadata regression.' }
    foreach ($tier in @('Low','Medium','High','Ultra')) { if (!$entry.search.Contains($entry.id + '_' + $tier)) { throw 'Missing legacy search alias' } }
}
$vsr = $catalog.effects | Where-Object id -eq 'RTXVideo\RTXVideo_VSR'
if ($vsr.name -ne 'Nvidia VFX VSR' -or $vsr.details -notmatch '去模糊' -or $vsr.details -notmatch 'Ampere') { throw 'Unified VSR category guidance regression' }
foreach ($item in @(@('NvidiaVFX\Upscale','Nvidia VFX Upscale'), @('NvidiaVFX\VFG','Nvidia VFX VFG'), @('RTXVideo\RTXVideo_HDR','Nvidia VFX TrueHDR'))) {
    if (($catalog.effects | Where-Object id -eq $item[0]).name -ne $item[1]) { throw "VFX suite name regression: $($item[0])" }
}
if (@($catalog.effects | Where-Object { $_.id -like 'XeSSFG\*' -and $_.name -like '*ZeroMV*' }).Count) { throw 'XeSS display alias regression' }
$fgNames = @{
    'DLSSFG\DLSS_FrameGeneration' = 'DLSS FrameGeneration'
    'XeSSFG\XeSS_FrameGeneration' = 'XeSS FrameGeneration'
    'FSR3FG\FSR3_FrameGeneration' = 'FSR3 FrameGeneration'
}
$helper = Get-Content -LiteralPath (Join-Path $repoRoot 'src/Magpie/EffectHelper.h') -Raw
foreach ($id in $fgNames.Keys) {
    $mapping = 'if (fullName == L"' + $id.Replace('\', '\\') + '") return L"' + $fgNames[$id] + '";'
    if (!$helper.Contains($mapping)) { throw "FG helper/catalog name mismatch: $id" }
    foreach ($languageCatalog in @($catalog, $englishCatalog)) {
        $entry = $languageCatalog.effects | Where-Object id -eq $id
        if ($entry.name -cne $fgNames[$id]) { throw "FG display name mismatch: $id" }
        if (!$entry.search.Contains($entry.name)) { throw "Missing current FG display name in search: $id" }
        if (!$entry.search.Contains(($id -split '\\')[-1])) { throw "Missing legacy FG filename search alias: $id" }
    }
}
foreach ($languageCatalog in @($catalog, $englishCatalog)) {
    $fsr = $languageCatalog.effects | Where-Object id -eq 'FSR3FG\FSR3_FrameGeneration'
    foreach ($alias in @('FSR3FG', 'FSR 3 FG', 'FSR3_FrameGeneration')) {
        if (!$fsr.search.Contains($alias)) { throw "Missing legacy FSR search alias: $alias" }
    }
    if ($fsr.summary -notmatch 'SDR' -or $fsr.summary -notmatch '2[×x]' -or
        $fsr.details -notmatch '实验性|Experimental') { throw 'FSR fixed scale, SDR scope and experimental status must remain visible.' }
}
$cunny = @($catalog.effects | Where-Object { $_.family.id -eq 'cunny' })
if ($cunny.Count -ne 29 -or @($cunny.subfamily.id | Sort-Object -Unique).Count -ne 2) { throw 'CuNNy generation grouping regression' }
if (@($cunny | Where-Object level -ne 'situational').Count) { throw 'Network cost is being labeled experimental again.' }
if (@($catalog.effects | Where-Object { $_.family.id -eq 'nnedi3' }).Count -ne 10) { throw 'NNEDI3 family regression' }
if (@($catalog.effects | Where-Object { $_.id -match '^(CRT|Sharpen|Diagnostics|RTXVideo)\\' -and $_.family }).Count) { throw 'Mixed algorithms were merged into a family' }
"Catalog validated: $($ids.Count) source effects, $($catalog.effects.Count) built-in picker entries; eight RTX names retained as aliases."
