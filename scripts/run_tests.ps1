$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Binary = Join-Path $RepoRoot "magnesium.exe"
$TestDir = Join-Path $RepoRoot "tests"
$ExpectedDir = Join-Path $RepoRoot "tests\expected"
$Timeout = if ($env:TIMEOUT) { [int]$env:TIMEOUT } else { 10 }

$Verbose = $false
$Filter = ""

foreach ($arg in $args) {
    switch ($arg) {
        "-v" { $Verbose = $true }
        default { $Filter = $arg }
    }
}

if (-not (Test-Path $Binary -PathType Leaf)) {
    Write-Host "Error: $Binary not found." -ForegroundColor Red
    Write-Host "Run 'make' first."
    exit 1
}

$Passed = 0
$Failed = 0
$Skipped = 0
$Errors = @()

Write-Host "Magnesium Test Runner"
Write-Host "----------------------------------------------"

$testFiles = Get-ChildItem -Path $TestDir -Filter "test_*.mg" | Sort-Object Name

foreach ($testFile in $testFiles) {
    $testName = $testFile.BaseName

    if ($Filter -and $testName -notlike "*$Filter*") { continue }

    $expectedFile = Join-Path $ExpectedDir "$testName.expected"
    if (-not (Test-Path $expectedFile -PathType Leaf)) {
        Write-Host "  SKIP  $testName (no expected output file)" -ForegroundColor Yellow
        $Skipped++
        continue
    }

    $expected = Get-Content $expectedFile -Raw

    $relativeTestFile = "tests\$($testFile.Name)"
    $proc = Start-Process -FilePath $Binary -ArgumentList $relativeTestFile -WorkingDirectory $RepoRoot -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$env:TEMP\mg_test_out.txt" -RedirectStandardError "$env:TEMP\mg_test_err.txt"

    $actual = if (Test-Path "$env:TEMP\mg_test_out.txt") { Get-Content "$env:TEMP\mg_test_out.txt" -Raw } else { "" }
    $stderr = if (Test-Path "$env:TEMP\mg_test_err.txt") { Get-Content "$env:TEMP\mg_test_err.txt" -Raw } else { "" }
    $actual = ($actual + $stderr).TrimEnd()
    $expected = $expected.TrimEnd()

    if ($proc.ExitCode -ge 128) {
        Write-Host "  FAIL  $testName (terminated abnormally)" -ForegroundColor Red
        $Failed++
        $Errors += "  - $testName : abnormal exit"
        continue
    }

    if ($actual -eq $expected) {
        Write-Host "  PASS  $testName" -ForegroundColor Green
        $Passed++
    } else {
        Write-Host "  FAIL  $testName" -ForegroundColor Red
        $Failed++
        $Errors += "  - $testName : output mismatch"
        if ($Verbose) {
            Write-Host ""
            Write-Host "    Expected:" -ForegroundColor Cyan
            $expected -split "`n" | ForEach-Object { Write-Host "    | $_" }
            Write-Host "    Actual:" -ForegroundColor Cyan
            $actual -split "`n" | ForEach-Object { Write-Host "    | $_" }
            Write-Host ""
        }
    }
}

$total = $Passed + $Failed + $Skipped
Write-Host "----------------------------------------------"
Write-Host "Results: $total tests | $Passed passed | $Failed failed | $Skipped skipped"

if ($Failed -gt 0) {
    Write-Host ""
    Write-Host "Failures:" -ForegroundColor Red
    $Errors | ForEach-Object { Write-Host $_ }
    exit 1
}
