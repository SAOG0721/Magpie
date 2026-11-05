#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet('Smoke','Acceptance')][string]$Suite='Smoke',
    [string]$FromCase
)
$ErrorActionPreference='Stop'
$probePath=(Resolve-Path -LiteralPath $Probe).Path
$outputPath=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath (Join-Path $outputPath 'run.json')) { throw 'Evidence directory is immutable; choose a fresh Output.' }
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$cases=[Collections.Generic.List[object]]::new()
foreach($mode in @(0,1,2,3)) {
    $intervals=if($mode -in @(1,3)){@(2,3,4)}else{@(2)}
    $scenes=if($Suite -eq 'Smoke'){@('translate')}else{@('static','translate','cut','occlusion')}
    foreach($interval in $intervals) { foreach($scene in $scenes) {
        $cases.Add(@{name="m$mode-k$interval-$scene";args=@('--mode',"$mode",'--interval',"$interval",'--flow','amd1','--scene',$scene)})
    }}
}
if($Suite -eq 'Acceptance') {
    foreach($mode in @(0,1,2,3)) {
        $cases.Add(@{name="m$mode-passes3-boundary";args=@('--mode',"$mode",'--interval','4','--flow','nv1','--scene','ui','--passes','3','--diverse-passes','--reset-at','18','--idle-after','30','--idle-ms','220')})
        $cases.Add(@{name="m$mode-ht75";args=@('--mode',"$mode",'--interval','3','--model','1','--percent','75','--flow','nv3','--scene','lighting')})
    }
    foreach($mode in @(1,3)) {
        foreach($percent in @(25,37,50,62,75,87,100)) {
            $cases.Add(@{name="m$mode-percent$percent-120";args=@('--mode',"$mode",'--interval','4','--flow','amd1','--scene','translate','--percent',"$percent",'--fps','120')})
        }
        foreach($flow in @('amd0','nv1','nv2','nv3','nv4','nv5')) {
            $cases.Add(@{name="m$mode-flow$flow";args=@('--mode',"$mode",'--interval','4','--flow',$flow,'--scene','translate')})
        }
    }
}
if($FromCase) {
    $start=-1
    for($i=0;$i -lt $cases.Count;$i++) {if($cases[$i].name -eq $FromCase){$start=$i;break}}
    if($start -lt 0){throw "Unknown case: $FromCase"}
    $cases=@($cases)[$start..($cases.Count-1)]
}
$manifest=[ordered]@{suite=$Suite;modeVersion=1;fromCase=$FromCase;probe=$probePath;probeSha256=(Get-FileHash -LiteralPath $probePath -Algorithm SHA256).Hash;started=(Get-Date).ToString('o');cases=@();status='running'}
$manifestPath=Join-Path $outputPath 'run.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
foreach($case in $cases) {
    $prefix=Join-Path $outputPath $case.name
    $arguments=@('--warmup','8','--samples',$(if($Suite -eq 'Smoke'){'40'}else{'72'}),'--fps','60','--tail-ms','300','--prefix',$prefix)+$case.args
    $started=Get-Date
    New-Item -ItemType Directory -Path "$prefix.runtime" -Force | Out-Null
    Push-Location "$prefix.runtime"
    try { & $probePath @arguments *> "$prefix.console.log"; $code=$LASTEXITCODE } finally { Pop-Location }
    $manifest.cases+=@{name=$case.name;arguments=$arguments;exitCode=$code;started=$started.ToString('o');elapsedSeconds=((Get-Date)-$started).TotalSeconds}
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    Write-Host "$($case.name): exit=$code"
    if($code) { $manifest.status='failed';$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8;throw "Four-mode matrix failed at $($case.name)." }
}
$manifest.status='completed';$manifest.finished=(Get-Date).ToString('o')
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
& python (Join-Path $PSScriptRoot 'analyze_dlssnr_four_modes.py') $outputPath --output (Join-Path $outputPath 'analysis.json')
if($LASTEXITCODE) { throw 'Four-mode evidence audit failed.' }
