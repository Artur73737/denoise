param([switch]$IncludeCachedModel)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$binRoot = Join-Path $projectRoot 'build\bin'
$packageRoot = Join-Path $projectRoot 'dist\denoise'
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
foreach ($name in @('denoise.exe', 'onnxruntime.dll', 'DirectML.dll', 'libraw.dll')) {
    Copy-Item -LiteralPath (Join-Path $binRoot $name) -Destination (Join-Path $packageRoot $name)
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $packageRoot
$licenseRoot = Join-Path $packageRoot 'licenses'
New-Item -ItemType Directory -Force -Path $licenseRoot | Out-Null
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'licenses') -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $licenseRoot }
if ($IncludeCachedModel) {
    $cacheRoot = Join-Path $packageRoot 'models'
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $binRoot 'models') -Filter 'scunet_*.onnx' -File -ErrorAction SilentlyContinue |
        ForEach-Object {
            $weights = $_.FullName + '.data'
            if (Test-Path -LiteralPath $weights -PathType Leaf) {
                Copy-Item -LiteralPath $_.FullName -Destination $cacheRoot
                Copy-Item -LiteralPath $weights -Destination $cacheRoot
            }
        }
}
$checksums = [ordered]@{}
Get-ChildItem -LiteralPath $packageRoot -Recurse -File |
    Where-Object Name -ne 'SHA256SUMS.json' | ForEach-Object {
        $relative = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
        $checksums[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
[System.IO.File]::WriteAllText((Join-Path $packageRoot 'SHA256SUMS.json'), ($checksums | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Write-Host "Package ready: $packageRoot"
