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

function Invoke-TestProcess {
    param(
        [string]$FilePath,
        [string]$Argument,
        [string]$WorkingDirectory,
        [int]$TimeoutSeconds
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = '"' + $Argument.Replace('"', '\"') + '"'
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    # Normalize case-insensitive Windows environment names. Some parent
    # processes expose both Path and PATH, which Start-Process rejects.
    $normalizedEnvironment = @{}
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $normalizedEnvironment[$entry.Key.ToString().ToUpperInvariant()] = [string]$entry.Value
    }
    $processEnvironment = $startInfo.Environment
    if ($null -eq $processEnvironment) {
        $processEnvironment = $startInfo.EnvironmentVariables
    }
    if ($null -ne $processEnvironment) {
        $processEnvironment.Clear()
        foreach ($key in $normalizedEnvironment.Keys) {
            $processEnvironment[$key] = $normalizedEnvironment[$key]
        }
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        $process.Dispose()
        throw "Failed to start $FilePath"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $exited = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $exited) {
        try { $process.Kill($true) } catch { try { $process.Kill() } catch { } }
        $process.WaitForExit()
    }
    $stdout = [string]$stdoutTask.Result
    $stderr = [string]$stderrTask.Result
    $exitCode = if ($exited) { $process.ExitCode } else { -1 }
    $process.Dispose()
    return [pscustomobject]@{
        Exited = $exited
        ExitCode = $exitCode
        Stdout = $stdout
        Stderr = $stderr
    }
}

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
    $run = Invoke-TestProcess -FilePath $Binary -Argument $relativeTestFile `
        -WorkingDirectory $RepoRoot -TimeoutSeconds $Timeout
    if (-not $run.Exited) {
        Write-Host "  FAIL  $testName (timed out after ${Timeout}s)" -ForegroundColor Red
        $Failed++
        $Errors += "  - $testName : timeout"
        continue
    }

    $actual = ([string]$run.Stdout + [string]$run.Stderr).
        Replace("`r`n", "`n").Replace("`r", "`n").TrimEnd()
    $expected = ([string]$expected).
        Replace("`r`n", "`n").Replace("`r", "`n").TrimEnd()

    if ($run.ExitCode -ge 128) {
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

$cliNulTest = "test_cli_embedded_nul"
if (-not $Filter -or $cliNulTest -like "*$Filter*") {
    $tempRoot = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath())
    $nulFile = [System.IO.Path]::GetTempFileName()
    try {
        $prefixBytes = [System.Text.Encoding]::UTF8.GetBytes(
            'print("before")')
        $suffixBytes = [System.Text.Encoding]::UTF8.GetBytes(
            'print("after")')
        $bytes = [byte[]]($prefixBytes + [byte]0 + $suffixBytes)
        [System.IO.File]::WriteAllBytes($nulFile, $bytes)
        $run = Invoke-TestProcess -FilePath $Binary -Argument $nulFile `
            -WorkingDirectory $RepoRoot -TimeoutSeconds $Timeout
        $nulStdout = ([string]$run.Stdout).Trim()
        $nulStderr = [string]$run.Stderr
        if ($run.Exited -and $run.ExitCode -eq 65 -and
                [string]::IsNullOrWhiteSpace($nulStdout) -and
                $nulStderr -match "embedded NUL byte" -and
                $nulStderr -notmatch "before|after") {
            Write-Host "  PASS  $cliNulTest" -ForegroundColor Green
            $Passed++
        } else {
            Write-Host "  FAIL  $cliNulTest" -ForegroundColor Red
            $Failed++
            $Errors += "  - $cliNulTest : expected exit 65 and embedded-NUL diagnostic"
            if ($Verbose) {
                Write-Host "    Exit code: $($run.ExitCode)"
                Write-Host "    Stdout: $nulStdout"
                Write-Host "    Stderr: $nulStderr"
            }
        }
    } finally {
        $resolvedNulFile = [System.IO.Path]::GetFullPath($nulFile)
        if ($resolvedNulFile.StartsWith(
                $tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedNulFile -Force `
                -ErrorAction SilentlyContinue
        } else {
            Write-Warning "Refusing to remove unexpected temporary path: $resolvedNulFile"
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
