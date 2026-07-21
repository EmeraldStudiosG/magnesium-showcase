$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BenchDir = Join-Path $RepoRoot "benchmark"
$Runs = if ($env:RUNS) { [int]$env:RUNS } else { 5 }
$BenchTimeout = if ($env:BENCH_TIMEOUT) { [int]$env:BENCH_TIMEOUT } else { 30 }
$BenchFilter = if ($env:BENCH) { $env:BENCH } else { "" }

$Benchmarks = @(
    "fib35", "binary_trees", "sieve", "mandelbrot", "dict_bench",
    "arith_loop", "call_loop", "closure_loop", "array_loop",
    "object_fields", "gc_alloc", "string_concat", "control_flow",
    "fallible_lookup", "native_len", "coroutine_switch"
)

if ($BenchFilter) {
    if ($Benchmarks -contains $BenchFilter) {
        $Benchmarks = @($BenchFilter)
    } else {
        Write-Host "Unknown benchmark: $BenchFilter"
        Write-Host "Available: $($Benchmarks -join ', ')"
        exit 1
    }
}

function Get-RunTime {
    param([string]$Cmd)
    # Launch the runtime directly (no cmd.exe wrapper) and redirect stdout/stderr
    # so benchmark output cannot corrupt the timing tables. Read the streams
    # asynchronously to avoid blocking when a program writes a lot to stdout.
    $parts = $Cmd -split '\s+', 2
    $exe = $parts[0]
    $argStr = if ($parts.Count -gt 1) { $parts[1] } else { "" }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $argStr
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $null = $proc.Start()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $exited = $proc.WaitForExit($BenchTimeout * 1000)
    $sw.Stop()

    $null = $outTask.Result
    $null = $errTask.Result

    if (-not $exited) {
        try { $proc.Kill() } catch { }
        $proc.Close()
        Write-Host "Benchmark timed out after ${BenchTimeout}s: $Cmd" -ForegroundColor Red
        exit 1
    }
    $code = $proc.ExitCode
    $proc.Close()
    if ($code -ne 0) {
        Write-Host "Benchmark command failed (exit $code): $Cmd" -ForegroundColor Red
        exit 1
    }
    $sw.Elapsed.TotalSeconds
}

Write-Host "=== Phase 0: Cleanup ==="
foreach ($b in $Benchmarks) {
    @("$BenchDir\$b.mgc", "$BenchDir\$b.luac", "$BenchDir\$b.luajit") | ForEach-Object {
        if (Test-Path $_) { Remove-Item $_ }
    }
}
if (Test-Path "$BenchDir\__pycache__") {
    foreach ($b in $Benchmarks) {
        Get-ChildItem "$BenchDir\__pycache__" -Filter "$b.cpython-*.pyc" -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }
}
Write-Host "Done.`n"

Write-Host "=== Phase 1: Preparation ==="
foreach ($b in $Benchmarks) {
    Write-Host ("Building {0,-15} ..." -f "$b (all languages)") -NoNewline
    & "$RepoRoot\magnesium.exe" build "$BenchDir\$b.mg" | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "Magnesium build failed."; exit 1 }
    luac -o "$BenchDir\$b.luac" "$BenchDir\$b.lua"
    if ($LASTEXITCODE -ne 0) { Write-Host "Lua build failed."; exit 1 }
    luajit -b "$BenchDir\$b.lua" "$BenchDir\$b.luajit"
    if ($LASTEXITCODE -ne 0) { Write-Host "LuaJIT build failed."; exit 1 }
    python3 -m py_compile "$BenchDir\$b.py"
    if ($LASTEXITCODE -ne 0) { Write-Host "Python compile failed."; exit 1 }
    Write-Host " Done."
}
Write-Host ""

Write-Host "=== Phase 2: Benchmarking ==="

foreach ($b in $Benchmarks) {
    Write-Host "--------------------------------------------------------"
    Write-Host "Benchmarking: $b`n"

    $interpRuntimes = [ordered]@{
        "Magnesium" = "$RepoRoot\magnesium.exe $BenchDir\$b.mg"
        "Lua 5.4"   = "lua $BenchDir\$b.lua"
        "LuaJIT"    = "luajit $BenchDir\$b.lua"
        "Python 3"  = "python3 $BenchDir\$b.py"
    }

    Write-Host "### $b - Interpreted / Source Mode ($Runs runs)"
    Write-Host "| Runtime          |"
    for ($i = 1; $i -le $Runs; $i++) { Write-Host " Run $i |" -NoNewline }
    Write-Host " **Avg** |"
    Write-Host "|------------------|$(("-" * 7 + "|") * $Runs)----------|"

    foreach ($kv in $interpRuntimes.GetEnumerator()) {
        $total = 0.0
        $times = @()
        for ($i = 1; $i -le $Runs; $i++) {
            $t = Get-RunTime $kv.Value
            $times += $t
            $total += $t
        }
        $avg = if ($Runs -gt 0) { $total / $Runs } else { 0.0 }
        Write-Host ("| {0,-16} |" -f $kv.Key) -NoNewline
        foreach ($t in $times) { Write-Host (" {0:F6}s |" -f $t) -NoNewline }
        Write-Host (" **{0:F6}s** |" -f $avg)
    }
    Write-Host ""

    $pyc = Get-ChildItem "$BenchDir\__pycache__\$b.cpython-*.pyc" | Select-Object -First 1

    $byteRuntimes = [ordered]@{
        "Magnesium (MGC)" = "$RepoRoot\magnesium.exe $BenchDir\$b.mgc"
        "Lua 5.4 (LUAC)" = "lua $BenchDir\$b.luac"
        "LuaJIT (Byte)"   = "luajit $BenchDir\$b.luajit"
        "Python 3 (PYC)"  = "python3 $($pyc.FullName)"
    }

    Write-Host "### $b - Compiled / Bytecode Mode ($Runs runs)"
    Write-Host "| Runtime          |"
    for ($i = 1; $i -le $Runs; $i++) { Write-Host " Run $i |" -NoNewline }
    Write-Host " **Avg** |"
    Write-Host "|------------------|$(("-" * 7 + "|") * $Runs)----------|"

    foreach ($kv in $byteRuntimes.GetEnumerator()) {
        $total = 0.0
        $times = @()
        for ($i = 1; $i -le $Runs; $i++) {
            $t = Get-RunTime $kv.Value
            $times += $t
            $total += $t
        }
        $avg = if ($Runs -gt 0) { $total / $Runs } else { 0.0 }
        Write-Host ("| {0,-16} |" -f $kv.Key) -NoNewline
        foreach ($t in $times) { Write-Host (" {0:F6}s |" -f $t) -NoNewline }
        Write-Host (" **{0:F6}s** |" -f $avg)
    }
    Write-Host ""
}
