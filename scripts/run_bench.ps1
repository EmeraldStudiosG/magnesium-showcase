$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BenchDir = Join-Path $RepoRoot "benchmark"

function Read-IntegerSetting {
    param(
        [string]$Name,
        [int]$Default,
        [int]$Minimum
    )

    $raw = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return $Default
    }

    $value = 0
    if (-not [int]::TryParse($raw, [ref]$value) -or $value -lt $Minimum) {
        throw "$Name must be an integer greater than or equal to $Minimum."
    }
    return $value
}

function Read-BoolSetting {
    param(
        [string]$Name,
        [bool]$Default = $false
    )

    $raw = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return $Default
    }
    switch ($raw.ToLowerInvariant()) {
        { @("1", "true", "yes", "on") -contains $_ } { return $true }
        { @("0", "false", "no", "off") -contains $_ } { return $false }
        default { throw "$Name must be a boolean setting (0/1 or false/true)." }
    }
}

function ConvertTo-ProcessArgument {
    param([AllowEmptyString()][string]$Value)

    if ($Value.Length -eq 0) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    if ($Value.Contains('"')) {
        throw "Process arguments containing a double quote are not supported: $Value"
    }

    # Windows command-line parsing requires trailing backslashes to be doubled
    # when the argument is quoted.
    $quoted = $Value -replace '(\\+)$', '$1$1'
    return '"' + $quoted + '"'
}

function New-ProcessStartInfo {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (($Arguments | ForEach-Object {
        ConvertTo-ProcessArgument ([string]$_)
    }) -join " ")
    $startInfo.WorkingDirectory = $RepoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    # Windows environment variables are case-insensitive, but a parent process
    # can still contain both Path and PATH. .NET rejects that duplicate when it
    # starts a child, so collapse the entries before launching it.
    $pathValue = [Environment]::GetEnvironmentVariable("PATH")
    try {
        foreach ($key in @($startInfo.Environment.Keys)) {
            if ([string]::Equals([string]$key, "PATH",
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                [void]$startInfo.Environment.Remove([string]$key)
            }
        }
        if ($null -ne $pathValue) {
            $startInfo.Environment["PATH"] = $pathValue
        }
    } catch {
        foreach ($key in @($startInfo.EnvironmentVariables.Keys)) {
            if ([string]::Equals([string]$key, "PATH",
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                [void]$startInfo.EnvironmentVariables.Remove([string]$key)
            }
        }
        if ($null -ne $pathValue) {
            $startInfo.EnvironmentVariables["PATH"] = $pathValue
        }
    }

    return $startInfo
}

function Invoke-CapturedProcess {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [int]$TimeoutSeconds
    )

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = New-ProcessStartInfo $FilePath $Arguments
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    try {
        if (-not $process.Start()) {
            throw "Could not start process: $FilePath"
        }

        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $exited = $process.WaitForExit($TimeoutSeconds * 1000)
        if (-not $exited) {
            try { $process.Kill() } catch { }
            $process.WaitForExit()
        }

        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        $stopwatch.Stop()

        return [pscustomobject]@{
            Exited = $exited
            ExitCode = if ($exited) { $process.ExitCode } else { $null }
            Stdout = if ($null -eq $stdout) { "" } else { [string]$stdout }
            Stderr = if ($null -eq $stderr) { "" } else { [string]$stderr }
            ElapsedSeconds = $stopwatch.Elapsed.TotalSeconds
        }
    } finally {
        if ($stopwatch.IsRunning) {
            $stopwatch.Stop()
        }
        $process.Dispose()
    }
}

function Resolve-Executable {
    param([string]$Name)

    if ([string]::IsNullOrWhiteSpace($Name)) {
        return $null
    }
    if (Test-Path -LiteralPath $Name -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Name).Path
    }

    $command = Get-Command -Name $Name -CommandType Application `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command) {
        return $null
    }
    return $command.Source
}

function Find-Runtime {
    param(
        [string]$DisplayName,
        [string]$Override,
        [object[]]$Candidates,
        [string[]]$VersionArguments,
        [string]$VersionPattern
    )

    if (-not [string]::IsNullOrWhiteSpace($Override)) {
        $Candidates = @([pscustomobject]@{
            Name = $Override
            Prefix = @()
        })
    }

    foreach ($candidate in $Candidates) {
        $executable = Resolve-Executable ([string]$candidate.Name)
        if ($null -eq $executable) {
            continue
        }

        $arguments = @($candidate.Prefix) + @($VersionArguments)
        try {
            $probe = Invoke-CapturedProcess $executable $arguments 10
        } catch {
            if (-not [string]::IsNullOrWhiteSpace($Override)) {
                throw "$DisplayName override could not be run: $($_.Exception.Message)"
            }
            continue
        }

        $version = (($probe.Stdout + "`n" + $probe.Stderr).Trim())
        if ($probe.Exited -and $probe.ExitCode -eq 0 -and
                $version -match $VersionPattern) {
            return [pscustomobject]@{
                Name = $DisplayName
                FilePath = $executable
                Prefix = @($candidate.Prefix)
                Version = ($version -replace '\s+', ' ').Trim()
            }
        }

        if (-not [string]::IsNullOrWhiteSpace($Override)) {
            throw "$DisplayName override is not a supported runtime. " +
                "Expected version output matching '$VersionPattern', got '$version'."
        }
    }

    return $null
}

function New-CommandSpec {
    param(
        [string]$Label,
        [object]$Runtime,
        [string[]]$Arguments
    )

    return [pscustomobject]@{
        Label = $Label
        FilePath = $Runtime.FilePath
        Arguments = @($Runtime.Prefix) + @($Arguments)
    }
}

function Format-Command {
    param([object]$Command)

    return ((@($Command.FilePath) + @($Command.Arguments) | ForEach-Object {
        $part = [string]$_
        if ($part -match '\s') {
            '"' + $part.Replace('"', '\"') + '"'
        } else {
            $part
        }
    }) -join " ")
}

function Normalize-Output {
    param([string]$Text)

    if ($null -eq $Text) {
        return ""
    }
    return (($Text -replace "`r`n", "`n") -replace "`r", "`n").TrimEnd(
        [char[]]"`n")
}

function Test-EquivalentOutput {
    param(
        [string]$Expected,
        [string]$Actual
    )

    if ($Expected -ceq $Actual) {
        return $true
    }

    $expectedLines = $Expected.Split(
        [string[]]@("`n"), [System.StringSplitOptions]::None)
    $actualLines = $Actual.Split(
        [string[]]@("`n"), [System.StringSplitOptions]::None)
    if ($expectedLines.Count -ne $actualLines.Count) {
        return $false
    }

    for ($lineIndex = 0; $lineIndex -lt $expectedLines.Count; $lineIndex++) {
        if ($expectedLines[$lineIndex] -ceq $actualLines[$lineIndex]) {
            continue
        }

        $expectedTokens = @([regex]::Split(
            $expectedLines[$lineIndex].Trim(), '\s+'))
        $actualTokens = @([regex]::Split(
            $actualLines[$lineIndex].Trim(), '\s+'))
        if ($expectedTokens.Count -ne $actualTokens.Count) {
            return $false
        }

        for ($tokenIndex = 0; $tokenIndex -lt $expectedTokens.Count;
                $tokenIndex++) {
            $expectedToken = $expectedTokens[$tokenIndex]
            $actualToken = $actualTokens[$tokenIndex]
            if ($expectedToken -ceq $actualToken) {
                continue
            }

            $expectedNumber = 0.0
            $actualNumber = 0.0
            $style = [System.Globalization.NumberStyles]::Float
            $culture = [System.Globalization.CultureInfo]::InvariantCulture
            $expectedIsNumber = [double]::TryParse(
                $expectedToken, $style, $culture, [ref]$expectedNumber)
            $actualIsNumber = [double]::TryParse(
                $actualToken, $style, $culture, [ref]$actualNumber)
            if (-not $expectedIsNumber -or -not $actualIsNumber) {
                return $false
            }

            # Accept textual representation differences such as 1, 1.0, and
            # 1e0, but require them to decode to the exact same double.
            if ($expectedNumber -cne $actualNumber) {
                return $false
            }
        }
    }

    return $true
}

function Assert-SuccessfulResult {
    param(
        [object]$Result,
        [object]$Command,
        [switch]$AllowStderr
    )

    $formatted = Format-Command $Command
    if (-not $Result.Exited) {
        throw "Command timed out: $formatted"
    }
    if ($Result.ExitCode -ne 0) {
        $detail = (Normalize-Output $Result.Stderr)
        throw "Command failed with exit code $($Result.ExitCode): $formatted`n$detail"
    }
    if (-not $AllowStderr -and
            -not [string]::IsNullOrWhiteSpace($Result.Stderr)) {
        throw "Command wrote unexpected stderr: $formatted`n$($Result.Stderr)"
    }
}

function Invoke-BuildStep {
    param(
        [string]$Label,
        [object]$Command,
        [int]$TimeoutSeconds
    )

    Write-Host ("  {0,-24} {1}" -f $Label, (Format-Command $Command))
    $result = Invoke-CapturedProcess $Command.FilePath $Command.Arguments `
        $TimeoutSeconds
    Assert-SuccessfulResult $result $Command -AllowStderr
}

function Invoke-CorrectnessCheck {
    param(
        [object]$Command,
        [string]$Expected,
        [int]$TimeoutSeconds
    )

    Write-Host ("  check {0,-20} {1}" -f $Command.Label,
        (Format-Command $Command))
    $result = Invoke-CapturedProcess $Command.FilePath $Command.Arguments `
        $TimeoutSeconds
    Assert-SuccessfulResult $result $Command
    $actual = Normalize-Output $result.Stdout
    if (-not (Test-EquivalentOutput $Expected $actual)) {
        throw "Output mismatch for $($Command.Label).`n" +
            "Expected:`n$Expected`nActual:`n$actual"
    }
}

function Get-Median {
    param([double[]]$Values)

    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Measure-Command {
    param(
        [object]$Command,
        [string]$Expected,
        [int]$Runs,
        [int]$Warmups,
        [int]$TimeoutSeconds
    )

    for ($index = 0; $index -lt $Warmups; $index++) {
        $warmup = Invoke-CapturedProcess $Command.FilePath $Command.Arguments `
            $TimeoutSeconds
        Assert-SuccessfulResult $warmup $Command
        $warmupOutput = Normalize-Output $warmup.Stdout
        if (-not (Test-EquivalentOutput -Expected $Expected `
                -Actual $warmupOutput)) {
            throw "Output changed during warmup for $($Command.Label)."
        }
    }

    $times = @()
    for ($index = 0; $index -lt $Runs; $index++) {
        $result = Invoke-CapturedProcess $Command.FilePath $Command.Arguments `
            $TimeoutSeconds
        Assert-SuccessfulResult $result $Command
        $timedOutput = Normalize-Output $result.Stdout
        if (-not (Test-EquivalentOutput -Expected $Expected `
                -Actual $timedOutput)) {
            throw "Output changed during timed run for $($Command.Label)."
        }
        $times += [double]$result.ElapsedSeconds
    }

    return [pscustomobject]@{
        Label = $Command.Label
        Times = @($times)
        Average = [double](($times | Measure-Object -Average).Average)
        Median = [double](Get-Median ([double[]]$times))
    }
}

function Write-Results {
    param(
        [string]$Title,
        [object[]]$Results
    )

    Write-Host ""
    Write-Host $Title
    $magnesiumResult = $Results | Where-Object {
        $_.Label -like "Magnesium*"
    } | Select-Object -First 1
    Write-Host ("{0,-23} {1,11} {2,11} {3,9}  {4}" -f
        "Runtime", "Average", "Median", "vs Mg", "Runs")
    foreach ($result in $Results) {
        $runText = (($result.Times | ForEach-Object {
            "{0:F6}s" -f $_
        }) -join ", ")
        $relative = $result.Median / $magnesiumResult.Median
        Write-Host ("{0,-23} {1,10:F6}s {2,10:F6}s {3,8:F2}x  {4}" -f
            $result.Label, $result.Average, $result.Median, $relative,
            $runText)
    }
}

function Get-PerformanceFailures {
    param(
        [string]$Benchmark,
        [string]$Mode,
        [object[]]$Results
    )

    $failures = @()
    $magnesiumResult = $Results | Where-Object {
        $_.Label -like "Magnesium*"
    } | Select-Object -First 1
    if ($null -eq $magnesiumResult) {
        return @("$Benchmark ($Mode): missing Magnesium timing")
    }

    foreach ($result in $Results) {
        if ($result -eq $magnesiumResult) {
            continue
        }
        if ($magnesiumResult.Median -ge $result.Median) {
            $relative = $result.Median / $magnesiumResult.Median
            $failures += ("{0} ({1}): Magnesium {2:F6}s did not beat " +
                "{3} {4:F6}s ({5:F2}x; must be >1.00x)") -f
                $Benchmark, $Mode, $magnesiumResult.Median, $result.Label,
                $result.Median, $relative
        }
    }
    return @($failures)
}

$Runs = Read-IntegerSetting "RUNS" 5 1
$Warmups = Read-IntegerSetting "WARMUPS" 1 0
$BenchTimeout = Read-IntegerSetting "BENCH_TIMEOUT" 30 1
$RequireAllRuntimes = Read-BoolSetting "REQUIRE_ALL_RUNTIMES" $true
$PerformanceGate = Read-BoolSetting "PERFORMANCE_GATE" $true
$BenchFilter = [Environment]::GetEnvironmentVariable("BENCH")

$Benchmarks = @(
    "fib35", "binary_trees", "sieve", "mandelbrot", "dict_bench",
    "arith_loop", "call_loop", "closure_loop", "array_loop",
    "object_fields", "gc_alloc", "string_concat", "control_flow",
    "fallible_lookup", "native_len", "coroutine_switch"
)

if (-not [string]::IsNullOrWhiteSpace($BenchFilter)) {
    if ($Benchmarks -notcontains $BenchFilter) {
        throw "Unknown benchmark '$BenchFilter'. Available: " +
            ($Benchmarks -join ", ")
    }
    $Benchmarks = @($BenchFilter)
}

$BinaryOverride = if (-not [string]::IsNullOrWhiteSpace($env:BINARY)) {
    $env:BINARY
} elseif (-not [string]::IsNullOrWhiteSpace($env:MAGNESIUM_BIN)) {
    $env:MAGNESIUM_BIN
} else {
    Join-Path $RepoRoot "magnesium.exe"
}

$Magnesium = Find-Runtime "Magnesium" $BinaryOverride @() @("--version") `
    '^Magnesium v'
if ($null -eq $Magnesium) {
    throw "Magnesium executable was not found. Set BINARY or MAGNESIUM_BIN."
}

$Lua = Find-Runtime "Lua 5.4" $env:LUA @(
    [pscustomobject]@{ Name = "lua5.4"; Prefix = @() },
    [pscustomobject]@{ Name = "lua54"; Prefix = @() },
    [pscustomobject]@{ Name = "lua"; Prefix = @() }
) @("-v") 'Lua 5\.4'

$Luac = Find-Runtime "Lua 5.4 compiler" $env:LUAC @(
    [pscustomobject]@{ Name = "luac5.4"; Prefix = @() },
    [pscustomobject]@{ Name = "luac54"; Prefix = @() },
    [pscustomobject]@{ Name = "luac"; Prefix = @() }
) @("-v") 'Lua 5\.4'

$Python = Find-Runtime "Python 3" $env:PYTHON @(
    [pscustomobject]@{ Name = "python3"; Prefix = @() },
    [pscustomobject]@{ Name = "python"; Prefix = @() },
    [pscustomobject]@{ Name = "py"; Prefix = @("-3") }
) @("--version") '^Python 3\.'

Write-Host "=== Benchmark release gate ==="
Write-Host "Runs: $Runs; warmups: $Warmups; timeout: ${BenchTimeout}s"
Write-Host "Performance gate: $PerformanceGate; require all runtimes: $RequireAllRuntimes"
Write-Host ("Magnesium: {0} ({1})" -f $Magnesium.FilePath,
    $Magnesium.Version)
if ($null -ne $Lua) {
    Write-Host ("Lua:       {0} ({1})" -f $Lua.FilePath, $Lua.Version)
} else {
    Write-Host "Lua:       unavailable (requires Lua 5.4; set LUA)"
}
if ($null -ne $Luac) {
    Write-Host ("Lua bytecode compiler: {0} ({1})" -f
        $Luac.FilePath, $Luac.Version)
} else {
    Write-Host "Lua bytecode compiler: unavailable (set LUAC)"
}
if ($null -ne $Python) {
    Write-Host ("Python:    {0} ({1})" -f $Python.FilePath,
        $Python.Version)
} else {
    Write-Host "Python:    unavailable (requires Python 3; set PYTHON)"
}

if ($RequireAllRuntimes -and
        ($null -eq $Lua -or $null -eq $Luac -or $null -eq $Python)) {
    throw "REQUIRE_ALL_RUNTIMES is enabled, but Lua 5.4, luac 5.4, " +
        "and Python 3 are not all available."
}

$ArtifactDir = Join-Path ([System.IO.Path]::GetTempPath()) (
    "magnesium-bench-" + [Guid]::NewGuid().ToString("N"))
$null = New-Item -ItemType Directory -Path $ArtifactDir
$Succeeded = $false

try {
    $magnesiumExtension = [System.IO.Path]::GetExtension(
        [string]$Magnesium.FilePath)
    if ([string]::IsNullOrWhiteSpace($magnesiumExtension)) {
        $magnesiumExtension = ".exe"
    }
    $stagedMagnesiumPath = Join-Path $ArtifactDir (
        "magnesium" + $magnesiumExtension)
    Copy-Item -LiteralPath $Magnesium.FilePath `
        -Destination $stagedMagnesiumPath
    $BenchmarkMagnesium = [pscustomobject]@{
        Name = $Magnesium.Name
        FilePath = $stagedMagnesiumPath
        Prefix = @($Magnesium.Prefix)
        Version = $Magnesium.Version
    }

    $PreparedBenchmarks = @()

    Write-Host ""
    Write-Host "=== Preparation ==="
    foreach ($benchmark in $Benchmarks) {
        Write-Host $benchmark
        $mgSource = Join-Path $BenchDir "$benchmark.mg"
        $luaSource = Join-Path $BenchDir "$benchmark.lua"
        $pythonSource = Join-Path $BenchDir "$benchmark.py"

        foreach ($source in @($mgSource, $luaSource, $pythonSource)) {
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "Missing benchmark source: $source"
            }
        }

        $tempMgSource = Join-Path $ArtifactDir "$benchmark.mg"
        $tempLuaSource = Join-Path $ArtifactDir "$benchmark.lua"
        $tempPythonSource = Join-Path $ArtifactDir "$benchmark.py"
        $mgBytecode = Join-Path $ArtifactDir "$benchmark.mgc"
        Copy-Item -LiteralPath $mgSource -Destination $tempMgSource
        Copy-Item -LiteralPath $luaSource -Destination $tempLuaSource
        Copy-Item -LiteralPath $pythonSource -Destination $tempPythonSource
        $mgBuild = New-CommandSpec "Magnesium build" $BenchmarkMagnesium @(
            "build", $tempMgSource
        )
        Invoke-BuildStep "Magnesium bytecode" $mgBuild $BenchTimeout
        if (-not (Test-Path -LiteralPath $mgBytecode -PathType Leaf)) {
            throw "Magnesium build did not create $mgBytecode"
        }

        $sourceCommands = @(
            (New-CommandSpec "Magnesium" $BenchmarkMagnesium @($tempMgSource))
        )
        $bytecodeCommands = @(
            (New-CommandSpec "Magnesium (MGC)" $BenchmarkMagnesium @(
                $mgBytecode
            ))
        )

        if ($null -ne $Lua) {
            $sourceCommands += New-CommandSpec "Lua 5.4" $Lua @(
                $tempLuaSource
            )
        }
        if ($null -ne $Lua -and $null -ne $Luac) {
            $luaBytecode = Join-Path $ArtifactDir "$benchmark.luac"
            $luaBuild = New-CommandSpec "Lua compile" $Luac @(
                "-o", $luaBytecode, $tempLuaSource
            )
            Invoke-BuildStep "Lua bytecode" $luaBuild $BenchTimeout
            $bytecodeCommands += New-CommandSpec "Lua 5.4 (LUAC)" $Lua @(
                $luaBytecode
            )
        }

        if ($null -ne $Python) {
            $pythonBytecode = Join-Path $ArtifactDir "$benchmark.pyc"
            $compileCode = "import py_compile,sys;" +
                "py_compile.compile(sys.argv[1],cfile=sys.argv[2]," +
                "doraise=True)"
            $pythonBuild = New-CommandSpec "Python compile" $Python @(
                "-c", $compileCode, $tempPythonSource, $pythonBytecode
            )
            Invoke-BuildStep "Python bytecode" $pythonBuild $BenchTimeout
            $sourceCommands += New-CommandSpec "Python 3" $Python @(
                $tempPythonSource
            )
            $bytecodeCommands += New-CommandSpec "Python 3 (PYC)" $Python @(
                $pythonBytecode
            )
        }

        $PreparedBenchmarks += [pscustomobject]@{
            Name = $benchmark
            SourceCommands = @($sourceCommands)
            BytecodeCommands = @($bytecodeCommands)
        }
    }

    Write-Host ""
    Write-Host "=== Correctness preflight ==="
    $ExpectedOutputs = @{}
    foreach ($prepared in $PreparedBenchmarks) {
        Write-Host $prepared.Name
        $baselineCommand = $prepared.SourceCommands[0]
        Write-Host ("  baseline {0,-17} {1}" -f
            $baselineCommand.Label, (Format-Command $baselineCommand))
        $baseline = Invoke-CapturedProcess $baselineCommand.FilePath `
            $baselineCommand.Arguments $BenchTimeout
        Assert-SuccessfulResult $baseline $baselineCommand
        $expected = Normalize-Output $baseline.Stdout
        $ExpectedOutputs[$prepared.Name] = $expected

        foreach ($command in $prepared.SourceCommands) {
            Invoke-CorrectnessCheck $command $expected $BenchTimeout
        }
        foreach ($command in $prepared.BytecodeCommands) {
            Invoke-CorrectnessCheck $command $expected $BenchTimeout
        }
    }

    Write-Host ""
    Write-Host "=== Timed benchmarks ==="
    $PerformanceFailures = @()
    foreach ($prepared in $PreparedBenchmarks) {
        Write-Host ""
        Write-Host "--------------------------------------------------------"
        Write-Host $prepared.Name
        $expected = [string]$ExpectedOutputs[$prepared.Name]

        $sourceResults = @()
        foreach ($command in $prepared.SourceCommands) {
            $sourceResults += Measure-Command $command $expected $Runs `
                $Warmups $BenchTimeout
        }
        Write-Results "$($prepared.Name) - source mode" $sourceResults
        if ($PerformanceGate) {
            $PerformanceFailures += @(Get-PerformanceFailures `
                $prepared.Name "source" $sourceResults)
        }

        $bytecodeResults = @()
        foreach ($command in $prepared.BytecodeCommands) {
            $bytecodeResults += Measure-Command $command $expected $Runs `
                $Warmups $BenchTimeout
        }
        Write-Results "$($prepared.Name) - bytecode mode" $bytecodeResults
        if ($PerformanceGate) {
            $PerformanceFailures += @(Get-PerformanceFailures `
                $prepared.Name "bytecode" $bytecodeResults)
        }
    }

    if ($PerformanceGate -and $PerformanceFailures.Count -gt 0) {
        Write-Host ""
        Write-Host "=== Performance gate failures ===" -ForegroundColor Red
        foreach ($failure in $PerformanceFailures) {
            Write-Host ("  - " + $failure) -ForegroundColor Red
        }
        throw "$($PerformanceFailures.Count) runtime comparisons failed."
    }

    $Succeeded = $true
} catch {
    Write-Host ""
    Write-Host ("Benchmark gate failed: " + $_.Exception.Message) `
        -ForegroundColor Red
} finally {
    if (Test-Path -LiteralPath $ArtifactDir) {
        Remove-Item -LiteralPath $ArtifactDir -Recurse -Force
    }
}

if (-not $Succeeded) {
    exit 1
}

Write-Host ""
Write-Host "Benchmark release gate passed."
