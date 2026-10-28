param([string]$GameDirectory,[switch]$ValidateOnly,[string]$RollbackReceipt)
$ErrorActionPreference='Stop'
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
function Assert-Exited{
    if(@(Get-Process -Name HTGame,mgpu_official_host -ErrorAction SilentlyContinue).Count){throw 'Exit the game and helper normally first'}
}
function Resolve-Inside([string]$Root,[string]$Relative){
    $rootPath=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    $target=[IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if([IO.Path]::IsPathRooted($Relative) -or -not $target.StartsWith($rootPath,[StringComparison]::OrdinalIgnoreCase)){throw 'Path escapes target folder'}
    return $target
}
if($RollbackReceipt){
    $receiptPath=(Resolve-Path -LiteralPath $RollbackReceipt).Path
    $receipt=Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if($receipt.Format -ne 'MGPU-NTE-install-v1' -or -not $receipt.Completed){throw 'Unsupported or incomplete rollback receipt'}
    $GameDirectory=$receipt.GameDirectory
    if(-not(Test-Path -LiteralPath (Join-Path $GameDirectory 'HTGame.exe'))){throw 'Game directory missing'}
    foreach($entry in $receipt.Files){
        $target=Resolve-Inside $GameDirectory $entry.Path
        if(-not(Test-Path -LiteralPath $target) -or (Hash $target) -ne $entry.After){throw "Externally changed file, rollback refused: $($entry.Path)"}
        if($entry.Before -and (Hash (Resolve-Inside (Split-Path $receiptPath -Parent) $entry.Backup)) -ne $entry.Before){throw 'Rollback backup hash mismatch'}
    }
    if($ValidateOnly){'PASS: rollback identities; no changes';return}
    Assert-Exited
    foreach($entry in @($receipt.Files)[($receipt.Files.Count-1)..0]){
        Assert-Exited
        $target=Resolve-Inside $GameDirectory $entry.Path
        if($entry.Before){
            $staged=$target+'.nte-restore-'+[guid]::NewGuid().ToString('N')
            Copy-Item -LiteralPath (Resolve-Inside (Split-Path $receiptPath -Parent) $entry.Backup) -Destination $staged
            [IO.File]::Replace($staged,$target,(Join-Path (Split-Path $receiptPath -Parent) ('rollback-replaced-'+[guid]::NewGuid().ToString('N')+'.bin')))
            if((Hash $target) -ne $entry.Before){throw 'Restored hash mismatch'}
        }else{Remove-Item -LiteralPath $target}
    }
    'PASS: rollback complete; user configuration and imported runtime remain untouched';return
}
if(-not $GameDirectory){throw 'Specify -GameDirectory containing HTGame.exe'}
$GameDirectory=(Resolve-Path -LiteralPath $GameDirectory).Path
$exe=Join-Path $GameDirectory 'HTGame.exe'
if(-not(Test-Path -LiteralPath $exe)){throw 'HTGame.exe not found'}
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-manifest.json') -Raw | ConvertFrom-Json
if((Hash $exe) -ne $manifest.GameExeSHA256){throw 'Game build differs from the tested build; integration must be reviewed before installing'}
$operations=@()
foreach($entry in $manifest.Files){
    $source=Resolve-Inside $PSScriptRoot ('payload/'+$entry.Path)
    if((Hash $source) -ne $entry.SHA256){throw "Package integrity failed: $($entry.Path)"}
    $target=Resolve-Inside $GameDirectory $entry.Path
    if($entry.Configuration -and (Test-Path -LiteralPath $target)){Write-Output "KEEP configuration: $($entry.Path)";continue}
    if((Test-Path -LiteralPath $target) -and (Get-Item -LiteralPath $target).IsReadOnly){throw "Readonly target: $($entry.Path)"}
    $operations+= [pscustomobject]@{Path=$entry.Path;Source=$source;Target=$target;Before=$(if(Test-Path -LiteralPath $target){Hash $target}else{$null});After=$entry.SHA256;Backup=$null}
}
if($ValidateOnly){"PASS: $($operations.Count) package files validated; no changes";return}
Assert-Exited
$backup=Join-Path $GameDirectory ('mgpu-nte-backups/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fffffff'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
for($index=0;$index -lt $operations.Count;$index++){
    if($operations[$index].Before){
        $operations[$index].Backup="before-$index.bin"
        Copy-Item -LiteralPath $operations[$index].Target -Destination (Join-Path $backup $operations[$index].Backup)
        if((Hash (Join-Path $backup $operations[$index].Backup)) -ne $operations[$index].Before){throw 'Backup integrity failed'}
    }
}
$receipt=[ordered]@{Format='MGPU-NTE-install-v1';Release=$manifest.Release;At=(Get-Date -Format o);GameDirectory=$GameDirectory;Completed=$false;Files=$operations}
$receiptPath=Join-Path $backup 'receipt.json'
$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding utf8
$written=@()
try{
    foreach($entry in $operations){
        Assert-Exited
        if($entry.Before){if((Hash $entry.Target) -ne $entry.Before){throw 'Target changed during installation'}}
        elseif(Test-Path -LiteralPath $entry.Target){throw 'Target unexpectedly appeared'}
        New-Item -ItemType Directory -Path (Split-Path $entry.Target -Parent) -Force | Out-Null
        $staged=$entry.Target+'.nte-new-'+[guid]::NewGuid().ToString('N')
        Copy-Item -LiteralPath $entry.Source -Destination $staged
        if($entry.Before){[IO.File]::Replace($staged,$entry.Target,(Join-Path $backup ('atomic-'+[guid]::NewGuid().ToString('N')+'.bin')))}else{[IO.File]::Move($staged,$entry.Target)}
        $written+=$entry
        if((Hash $entry.Target) -ne $entry.After){throw 'Installed file integrity failed'}
    }
    $receipt.Completed=$true
}catch{
    $failure=$_
    [array]::Reverse($written)
    foreach($entry in $written){
        Assert-Exited
        if((Hash $entry.Target) -ne $entry.After){throw "External change during recovery: $($entry.Path)"}
        if($entry.Before){
            $restore=$entry.Target+'.nte-recover-'+[guid]::NewGuid().ToString('N')
            Copy-Item -LiteralPath (Join-Path $backup $entry.Backup) -Destination $restore
            [IO.File]::Replace($restore,$entry.Target,(Join-Path $backup ('recovery-replaced-'+[guid]::NewGuid().ToString('N')+'.bin')))
        }else{Remove-Item -LiteralPath $entry.Target}
    }
    throw $failure
}finally{$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding utf8}
"PASS: installed $($manifest.Release); rollback receipt=$receiptPath"
'Next: supply matching NVIDIA runtimes, merge Engine.ini example, and start from your original launcher.'
