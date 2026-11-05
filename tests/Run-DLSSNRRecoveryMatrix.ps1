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
if(Test-Path -LiteralPath (Join-Path $outputPath 'run.json')) { throw 'Completed or attempted matrix directories are immutable; choose a fresh Output.' }
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$cases=[Collections.Generic.List[object]]::new()
foreach($mode in @(1,4,6)) {
    foreach($policy in @(0,1,2)) {
        $scenes=if($Suite -eq 'Smoke') {@('translate')} else {@('static','translate','cut','occlusion')}
        foreach($scene in $scenes) {
            $cases.Add(@{name="m$mode-p$policy-$scene";args=@('--mode',"$mode",'--recovery',"$policy",'--flow',$(if($mode -eq 4){'amd1'}else{'nv1'}),'--scene',$scene)})
        }
    }
}
if($Suite -eq 'Acceptance') {
    foreach($mode in @(1,4,6)) {
        foreach($policy in @(0,1)) {
            $cases.Add(@{name="m$mode-p$policy-passes3-boundary";args=@('--mode',"$mode",'--recovery',"$policy",'--flow','nv1','--scene','ui','--passes','3','--diverse-passes','--reset-at','18','--idle-after','30','--idle-ms','220')})
        }
    }
    foreach($percent in @(25,37,50,62,75,87,100)) {
        $cases.Add(@{name="m4-p1-percent$percent-120";args=@('--mode','4','--recovery','1','--flow','amd1','--scene','translate','--percent',"$percent",'--fps','120','--interval','4','--buffer-delay','1')})
    }
    foreach($mode in @(1,4,6)) {
        $cases.Add(@{name="m$mode-p0-ht75";args=@('--mode',"$mode",'--recovery','0','--model','1','--percent','75','--flow','nv3','--scene','lighting')})
    }
    foreach($mode in @(0,2,3,5)) {
        $cases.Add(@{name="legacy-m$mode";args=@('--mode',"$mode",'--flow','amd1','--scene','translate')})
    }
}
if($FromCase) {
    $start=-1
    for($i=0;$i -lt $cases.Count;$i++) {if($cases[$i].name -eq $FromCase){$start=$i;break}}
    if($start -lt 0) {throw "Unknown case: $FromCase"}
    $cases=@($cases)[$start..($cases.Count-1)]
}
$manifest=[ordered]@{suite=$Suite;fromCase=$FromCase;probe=$probePath;probeSha256=(Get-FileHash -LiteralPath $probePath -Algorithm SHA256).Hash;started=(Get-Date).ToString('o');cases=@();status='running'}
$manifestPath=Join-Path $outputPath 'run.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
foreach($case in $cases) {
    $prefix=Join-Path $outputPath $case.name
    $arguments=@('--warmup','8','--samples',$(if($Suite -eq 'Smoke'){'40'}else{'72'}),'--fps','60','--tail-ms','300','--prefix',$prefix)+$case.args
    $started=Get-Date
    New-Item -ItemType Directory -Path "$prefix.runtime" -Force | Out-Null
    Push-Location "$prefix.runtime"
    try {
        & $probePath @arguments *> "$prefix.console.log"
        $code=$LASTEXITCODE
    } finally { Pop-Location }
    $manifest.cases+=@{name=$case.name;arguments=$arguments;exitCode=$code;started=$started.ToString('o');elapsedSeconds=((Get-Date)-$started).TotalSeconds}
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    Write-Host "$($case.name): exit=$code"
    if($code) { $manifest.status='failed';$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8;throw "Recovery matrix failed at $($case.name); inspect its console log." }
}
$manifest.status='completed';$manifest.finished=(Get-Date).ToString('o')
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
& python (Join-Path $PSScriptRoot 'analyze_dlssnr_recovery_policies.py') $outputPath --output (Join-Path $outputPath 'analysis.json')
if($LASTEXITCODE) { throw 'Recovery evidence audit failed.' }
