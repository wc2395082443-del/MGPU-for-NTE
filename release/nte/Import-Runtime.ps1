param([Parameter(Mandatory=$true)][string]$RuntimeSource,[Parameter(Mandatory=$true)][string]$GameDirectory,[switch]$ValidateOnly)
$ErrorActionPreference='Stop'
$RuntimeSource=(Resolve-Path -LiteralPath $RuntimeSource).Path
$GameDirectory=(Resolve-Path -LiteralPath $GameDirectory).Path
if(-not(Test-Path -LiteralPath (Join-Path $GameDirectory 'HTGame.exe'))){throw 'HTGame.exe not found'}
$requirements=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'runtime-requirements.json') -Raw | ConvertFrom-Json
$files=@(foreach($entry in $requirements.Files){
    if([IO.Path]::GetFileName($entry.Name) -ne $entry.Name){throw 'Invalid runtime requirement filename'}
    $source=Join-Path $RuntimeSource $entry.Name
    if(-not(Test-Path -LiteralPath $source) -or (Get-FileHash -LiteralPath $source).Hash -ne $entry.SHA256){throw "Tested runtime missing or different: $($entry.Name)"}
    $signature=Get-AuthenticodeSignature -LiteralPath $source
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA'){throw "NVIDIA signature invalid: $($entry.Name)"}
    [pscustomobject]@{Name=$entry.Name;Source=$source;SHA256=$entry.SHA256}
})
if($ValidateOnly){'PASS: all exact tested NVIDIA runtime identities/signatures matched; no copies';return}
if(@(Get-Process -Name HTGame,mgpu_official_host -ErrorAction SilentlyContinue).Count){throw 'Exit game/helper normally first'}
$destination=Join-Path $GameDirectory 'mgpu-official/runtime'
$backup=Join-Path $GameDirectory ('mgpu-nte-backups/runtime-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fffffff'))
New-Item -ItemType Directory -Path $destination,$backup -Force | Out-Null
$receipt=@(foreach($entry in $files){
    $target=Join-Path $destination $entry.Name
    $before=$null
    if(Test-Path -LiteralPath $target){
        $before=(Get-FileHash -LiteralPath $target).Hash
        Copy-Item -LiteralPath $target -Destination (Join-Path $backup $entry.Name)
    }
    [ordered]@{Name=$entry.Name;Before=$before;After=$entry.SHA256}
})
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'receipt.json') -Encoding utf8
foreach($entry in $files){
    if(@(Get-Process -Name HTGame,mgpu_official_host -ErrorAction SilentlyContinue).Count){throw 'Game/helper started during runtime import'}
    $target=Join-Path $destination $entry.Name
    $staged=$target+'.nte-runtime-'+[guid]::NewGuid().ToString('N')
    Copy-Item -LiteralPath $entry.Source -Destination $staged
    if(Test-Path -LiteralPath $target){[IO.File]::Replace($staged,$target,(Join-Path $backup ('atomic-'+$entry.Name)))}else{[IO.File]::Move($staged,$target)}
    if((Get-FileHash -LiteralPath $target).Hash -ne $entry.SHA256){throw 'Runtime import integrity failure'}
}
"PASS: exact tested signed runtime imported locally; previous files backed up in $backup"
