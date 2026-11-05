$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $projectRoot 'third_party'
New-Item -ItemType Directory -Force -Path $depsRoot | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Install-PackageFiles([string]$Id, [string]$Version, [string]$Hash, [string]$Folder) {
    $archive = Join-Path $depsRoot "$Id-$Version.nupkg"
    if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Hash) {
        $url = "https://api.nuget.org/v3-flatcontainer/$Id/$Version/$Id.$Version.nupkg"
        Write-Host "Download $Id $Version"
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Hash) {
        throw "SHA-256 mismatch: $Id"
    }
    $destination = Join-Path $depsRoot $Folder
    if (!(Test-Path -LiteralPath $destination)) {
        [System.IO.Compression.ZipFile]::ExtractToDirectory($archive, $destination)
    }
}
Install-PackageFiles 'microsoft.ml.onnxruntime.directml' '1.24.1' 'd5521507b32c638f1f29de42cb18f21182d40a7541398786d44e38f4a1d869f1' 'onnxruntime'
Install-PackageFiles 'microsoft.ai.directml' '1.15.4' '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9' 'directml'
$rawArchive = Join-Path $depsRoot 'LibRaw-0.22.3-Win64.zip'
$rawHash = '4e2213a0b10e32199a30b9114a61376cdb40f94df861262b4a6dca659701276b'
if (!(Test-Path -LiteralPath $rawArchive) -or (Get-FileHash -LiteralPath $rawArchive -Algorithm SHA256).Hash -ne $rawHash) {
    Invoke-WebRequest -Uri 'https://www.libraw.org/data/LibRaw-0.22.3-Win64.zip' -OutFile $rawArchive -UseBasicParsing
}
if ((Get-FileHash -LiteralPath $rawArchive -Algorithm SHA256).Hash -ne $rawHash) { throw 'LibRaw SHA-256 mismatch' }
$rawRoot = Join-Path $depsRoot 'libraw'
if (!(Test-Path -LiteralPath $rawRoot)) { [System.IO.Compression.ZipFile]::ExtractToDirectory($rawArchive, $rawRoot) }
Write-Host 'Native dependencies ready. The application needs no Python or .NET runtime.'
