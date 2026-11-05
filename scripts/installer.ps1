param([string]$RuntimeDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$versionText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($versionText -notmatch 'project\(photo_denoise VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'Project version not found' }
$version = $Matches[1]
$binRoot = Join-Path $projectRoot 'build\bin'
$stageRoot = Join-Path $projectRoot 'build\installer\payload'
$workRoot = Join-Path $projectRoot 'build\installer'
$releaseRoot = Join-Path $projectRoot 'dist\releases'
$wixRoot = Join-Path $projectRoot 'third_party\wix'
$wix = Join-Path $wixRoot 'wix.exe'
New-Item -ItemType Directory -Force -Path $stageRoot, $releaseRoot | Out-Null
if (!(Test-Path -LiteralPath $wix)) {
    dotnet tool install wix --version 5.0.2 --tool-path $wixRoot
    if ($LASTEXITCODE -ne 0) { throw 'WiX installation failed' }
}
if ((& $wix --version) -notmatch '^5\.0\.2\+') { throw 'This installer requires WiX 5.0.2' }
if (!$RuntimeDirectory) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vsRoot) { throw 'Visual Studio C++ tools not found; specify -RuntimeDirectory' }
    $redistRoot = Join-Path $vsRoot 'VC\Redist\MSVC'
    $RuntimeDirectory = Get-ChildItem -LiteralPath $redistRoot -Directory |
        Where-Object Name -match '^\d+\.\d+\.\d+$' | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (!$RuntimeDirectory) { throw 'Redistributable x64 C++ runtime not found' }
# Explicit allowlist: no weights, local evidence, stale downloads or test tools.
$payload = [ordered]@{}
foreach ($name in @('denoise.exe', 'onnxruntime.dll', 'DirectML.dll', 'libraw.dll')) {
    $payload[$name] = Join-Path $binRoot $name
}
foreach ($name in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
                    'msvcp140_codecvt_ids.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    $payload[$name] = Join-Path $RuntimeDirectory $name
}
$payload['README.md'] = Join-Path $projectRoot 'README.md'
$payload['models\catalog.json'] = Join-Path $projectRoot 'catalog\models.json'
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'licenses') -File | ForEach-Object {
    $payload[('licenses\' + $_.Name)] = $_.FullName
}
$xml = [System.Text.StringBuilder]::new()
[void]$xml.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><Fragment><ComponentGroup Id="Payload">')
$index = 0
foreach ($relative in $payload.Keys) {
    $source = $payload[$relative]
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing payload: $source" }
    $destination = Join-Path $stageRoot $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $directory = if ($relative.StartsWith('models\')) { 'MODELSFOLDER' } elseif ($relative.StartsWith('licenses\')) { 'LICENSESFOLDER' } else { 'INSTALLFOLDER' }
    $id = 'Payload_' + [System.IO.Path]::GetFileName($relative).Replace('.', '_').Replace('-', '_')
    $escaped = [System.Security.SecurityElement]::Escape($destination)
    $registry = [System.Security.SecurityElement]::Escape($relative)
    # Stable component identity across releases; keypath is HKCU for per-user files.
    $identity = [System.Text.Encoding]::UTF8.GetBytes("PhotoDenoise/Payload/$relative")
    $digest = [System.Security.Cryptography.SHA256]::Create().ComputeHash($identity)
    $guidBytes = New-Object byte[] 16
    [Array]::Copy($digest, $guidBytes, 16)
    $guid = [guid]::new($guidBytes).ToString('B').ToUpperInvariant()
    [void]$xml.AppendLine("<Component Id=`"$id`" Directory=`"$directory`" Guid=`"$guid`">")
    [void]$xml.AppendLine("<File Id=`"File_$index`" Source=`"$escaped`" KeyPath=`"no`" />")
    [void]$xml.AppendLine("<RegistryValue Root=`"HKCU`" Key=`"Software\Artur73737\PhotoDenoise\Files`" Name=`"$registry`" Type=`"integer`" Value=`"1`" KeyPath=`"yes`" />")
    [void]$xml.AppendLine('</Component>')
    $index++
}
[void]$xml.AppendLine('</ComponentGroup></Fragment></Wix>')
$payloadXml = Join-Path $workRoot 'Payload.wxs'
[System.IO.File]::WriteAllText($payloadXml, $xml.ToString(), [System.Text.UTF8Encoding]::new($false))
$msi = Join-Path $releaseRoot "PhotoDenoise-$version-x64.msi"
& $wix build (Join-Path $projectRoot 'installer\Package.wxs') $payloadXml -arch x64 -d "Version=$version" -intermediateFolder $workRoot -o $msi
if ($LASTEXITCODE -ne 0) { throw 'MSI build or Windows Installer validation failed' }
$hash = (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$msi.sha256", "$hash  $([System.IO.Path]::GetFileName($msi))`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Installer ready: $msi"
Write-Host "SHA-256: $hash"
