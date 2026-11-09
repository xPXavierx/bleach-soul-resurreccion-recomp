param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$Cache
)
$ErrorActionPreference = 'Stop'
$lock = Get-Content (Join-Path $PSScriptRoot 'build-inputs.json') -Raw | ConvertFrom-Json
if (Test-Path -LiteralPath $Destination) { throw 'Use a new, empty tool directory for each build.' }
$root = (New-Item -ItemType Directory -Path $Destination).FullName
if (-not $Cache) { $Cache = Join-Path $root 'downloads' }
New-Item -ItemType Directory -Path $Cache -Force | Out-Null
$Cache = (Resolve-Path -LiteralPath $Cache).Path
foreach ($entry in $lock.downloads.PSObject.Properties) {
    $spec = $entry.Value
    $file = Join-Path $Cache $spec.file
    if (-not (Test-Path -LiteralPath $file)) {
        Invoke-WebRequest -Uri $spec.url -OutFile $file
    }
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $spec.sha256) { throw "SHA-256 mismatch: $($spec.file): $actual" }
    Write-Output "Verified $($spec.file): $actual"
}
# Verify every archive before running or extracting anything. Do not use machine-wide tools.
foreach ($name in @('python', 'roslyn', 'net48')) {
    [System.IO.Compression.ZipFile]::ExtractToDirectory(
        (Join-Path $Cache $lock.downloads.$name.file), (Join-Path $root $name))
}
$extractor = Join-Path $Cache $lock.downloads.'7zr'.file
foreach ($name in @('rpcs3', 'ffmpeg')) {
    & $extractor x (Join-Path $Cache $lock.downloads.$name.file) "-o$(Join-Path $root $name)" -y
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $name" }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-inputs.json') -Destination (Join-Path $root 'build-inputs.json')
