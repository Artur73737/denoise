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
    $catalogPath = Join-Path $binRoot 'models\catalog.json'
    if (!(Test-Path -LiteralPath $catalogPath)) { $catalogPath = Join-Path $projectRoot 'catalog\models.json' }
    $catalog = Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json
    foreach ($model in $catalog.models) {
        $ready = $true
        foreach ($artifact in $model.files) {
            if ($artifact.name -notmatch '^[A-Za-z0-9_.-]+$' -or $artifact.name.Contains('..')) { throw 'Invalid catalog artifact filename' }
            $source = Join-Path (Join-Path $binRoot 'models') $artifact.name
            if (!(Test-Path -LiteralPath $source -PathType Leaf) -or (Get-Item -LiteralPath $source).Length -ne $artifact.bytes) { $ready = $false; break }
            if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $artifact.sha256) { $ready = $false; break }
        }
        if ($ready) {
            foreach ($artifact in $model.files) {
                Copy-Item -LiteralPath (Join-Path (Join-Path $binRoot 'models') $artifact.name) -Destination $cacheRoot
            }
        }
    }
    Copy-Item -LiteralPath $catalogPath -Destination (Join-Path $cacheRoot 'catalog.json')
}
$checksums = [ordered]@{}
Get-ChildItem -LiteralPath $packageRoot -Recurse -File |
    Where-Object Name -ne 'SHA256SUMS.json' | ForEach-Object {
        $relative = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
        $checksums[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
[System.IO.File]::WriteAllText((Join-Path $packageRoot 'SHA256SUMS.json'), ($checksums | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Write-Host "Package ready: $packageRoot"
