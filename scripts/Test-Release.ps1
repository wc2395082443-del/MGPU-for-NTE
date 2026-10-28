param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference='Stop'
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('mgpu-nte-release-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$package=Join-Path $fixture 'package'
$game=Join-Path $fixture 'game'
Copy-Item -LiteralPath $PackageDirectory -Destination $package -Recurse
New-Item -ItemType Directory -Path $game | Out-Null
$exe=Join-Path $game 'HTGame.exe'
[IO.File]::WriteAllText($exe,'synthetic game identity fixture')
$manifestPath=Join-Path $package 'package-manifest.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$manifest.GameExeSHA256=(Get-FileHash -LiteralPath $exe).Hash
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$original=Join-Path $game 'nvngx.dll_mgpu_bridge.addon64'
[IO.File]::WriteAllText($original,'old add-on fixture')
$oldHash=(Get-FileHash -LiteralPath $original).Hash
$config=Join-Path $game 'mgpu.ini'
[IO.File]::WriteAllText($config,"[MGPU]`nUserFixture=preserve`n")
$configHash=(Get-FileHash -LiteralPath $config).Hash
$installer=Join-Path $package 'Install.ps1'
& $installer -GameDirectory $game -ValidateOnly
if((Get-FileHash -LiteralPath $original).Hash -ne $oldHash){throw 'ValidateOnly mutated target'}
& $installer -GameDirectory $game
if((Get-FileHash -LiteralPath $config).Hash -ne $configHash){throw 'Existing configuration overwritten'}
$installedHash=(Get-FileHash -LiteralPath $original).Hash
if($installedHash -eq $oldHash){throw 'Replacement did not occur'}
$receipt=(Get-ChildItem -LiteralPath (Join-Path $game 'mgpu-nte-backups') -Filter receipt.json -Recurse | Select-Object -First 1).FullName
if(-not $receipt){throw 'Backup receipt missing'}
& $installer -RollbackReceipt $receipt -ValidateOnly
if((Get-FileHash -LiteralPath $original).Hash -ne $installedHash){throw 'Rollback ValidateOnly mutated target'}
$real=[IO.File]::ReadAllBytes($original)
[IO.File]::WriteAllText($original,'external change fixture')
$rejected=$false
try{& $installer -RollbackReceipt $receipt}catch{$rejected=$true}
if(-not $rejected){throw 'Rollback overwrote external modification'}
[IO.File]::WriteAllBytes($original,$real)
& $installer -RollbackReceipt $receipt
if((Get-FileHash -LiteralPath $original).Hash -ne $oldHash){throw 'Original add-on not restored'}
if((Get-FileHash -LiteralPath $config).Hash -ne $configHash){throw 'Rollback changed preserved configuration'}
if(Test-Path -LiteralPath (Join-Path $game 'mgpu-official/mgpu_official_host.exe')){throw 'New helper not removed during rollback'}
$payload=Join-Path $package 'payload/nvngx.dll_mgpu_bridge.addon64'
[IO.File]::WriteAllText($payload,'corrupt payload fixture')
$rejected=$false
try{& $installer -GameDirectory $game -ValidateOnly}catch{$rejected=$true}
if(-not $rejected){throw 'Corrupt package accepted'}
$manifest.Files[0].Path='../escaped.bin'
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$rejected=$false
try{& $installer -GameDirectory $game -ValidateOnly}catch{$rejected=$true}
if(-not $rejected){throw 'Path traversal accepted'}
"PASS: install/rollback, validation-only, preserved config, external-change rejection, corrupt-package and path containment. Fixture retained: $fixture"
