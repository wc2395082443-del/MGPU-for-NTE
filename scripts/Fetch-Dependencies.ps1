param([string]$HelperSDKDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$headers=@{'User-Agent'='MGPU-for-NTE-build'}
$dependencies=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
foreach($entry in $dependencies){
    $target=Join-Path $root $entry.Path
    if((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target).Hash -eq $entry.SHA256){continue}
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    $temporary=$target+'.download'
    Invoke-WebRequest -Uri $entry.URL -OutFile $temporary -Headers $headers
    if((Get-FileHash -LiteralPath $temporary).Hash -ne $entry.SHA256){throw "Dependency hash mismatch: $($entry.Path)"}
    Move-Item -LiteralPath $temporary -Destination $target -Force
}
if(-not $HelperSDKDirectory){$HelperSDKDirectory=Join-Path $root '_dl/streamline-sdk'}
$revision='625db07d4bca5abc67d781c42af7e7fb0761f1b6'
$marker=Join-Path $HelperSDKDirectory 'MGPU-NTE-REVISION.txt'
if(-not(Test-Path -LiteralPath $marker) -or (Get-Content -LiteralPath $marker -Raw).Trim() -ne $revision){
    if(Test-Path -LiteralPath $HelperSDKDirectory){throw 'Unrecognized SDK folder; select a new empty destination'}
    New-Item -ItemType Directory -Path (Split-Path $HelperSDKDirectory -Parent) -Force | Out-Null
    $archive=Join-Path $root '_dl/streamline-source.zip'
    Invoke-WebRequest -Uri "https://codeload.github.com/NVIDIA-RTX/Streamline/zip/$revision" -OutFile $archive -Headers $headers
    $expanded=Join-Path $root ('_dl/streamline-expand-'+[guid]::NewGuid().ToString('N'))
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    Move-Item -LiteralPath (Join-Path $expanded "Streamline-$revision") -Destination $HelperSDKDirectory
    Set-Content -LiteralPath $marker -Value $revision -Encoding ascii
}
if(-not(Test-Path -LiteralPath (Join-Path $HelperSDKDirectory 'include/sl_dlss_g.h'))){throw 'Incomplete Streamline SDK headers'}
"PASS: pinned main dependencies; helper SDK=$HelperSDKDirectory"
