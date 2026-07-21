$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

Set-Location (Join-Path $RepoRoot "docs")

if (-not (Get-Command mdbook -ErrorAction SilentlyContinue)) {
    Write-Host "Error: mdbook not found. Install with: cargo install mdbook" -ForegroundColor Red
    exit 1
}

$env:MG_BINARY = Join-Path $RepoRoot "magnesium.exe"
Write-Host "Starting Magnesium docs at http://0.0.0.0:3000"
mdbook serve -n 0.0.0.0 -p 3000
