$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Binary = if ($env:BINARY) { $env:BINARY } else { Join-Path $RepoRoot "magnesium.exe" }
$Timeout = if ($env:TIMEOUT) { [int]$env:TIMEOUT } else { 5 }

function Quote-ProcessArgument {
    param([string]$Value)
    if ($Value.IndexOf([char]0) -ge 0) { throw "Process argument contains NUL" }
    if ($Value.IndexOf('"') -ge 0) { throw "Process arguments containing quotes are not supported" }
    $trailingSlashes = 0
    for ($i = $Value.Length - 1; $i -ge 0 -and $Value[$i] -eq [char]92; $i--) {
        $trailingSlashes++
    }
    $prefixLength = $Value.Length - $trailingSlashes
    $escaped = $Value.Substring(0, $prefixLength)
    if ($trailingSlashes -gt 0) {
        $escaped += [string]::new([char]92, $trailingSlashes * 2)
    }
    return '"' + $escaped + '"'
}

function Invoke-CapturedProcess {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [int]$TimeoutSeconds
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (($Arguments | ForEach-Object {
        Quote-ProcessArgument ([string]$_)
    }) -join " ")
    $startInfo.WorkingDirectory = [string]$RepoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

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

function Find-Python {
    $candidates = @()
    if ($env:PYTHON) {
        $candidates += @{ Exe = $env:PYTHON; Prefix = @() }
    }
    $candidates += @(
        @{ Exe = "python3"; Prefix = @() },
        @{ Exe = "python"; Prefix = @() },
        @{ Exe = "py"; Prefix = @("-3") }
    )
    foreach ($candidate in $candidates) {
        if (-not (Get-Command $candidate.Exe -ErrorAction SilentlyContinue)) { continue }
        try {
            $probe = Invoke-CapturedProcess -FilePath $candidate.Exe `
                -Arguments @($candidate.Prefix + @("-c", "import sys")) `
                -TimeoutSeconds $Timeout
            if ($probe.Exited -and $probe.ExitCode -eq 0) { return $candidate }
        } catch {
            continue
        }
    }
    throw "Python 3 was not found (tried python3, python, and py -3)"
}

$TmpDir = Join-Path $env:TEMP "mg_bytecode_test_$(Get-Random)"
New-Item -ItemType Directory -Path $TmpDir | Out-Null

try {
    if (-not (Test-Path $Binary -PathType Leaf)) {
        Write-Host "Error: $Binary not found."
        exit 1
    }

    $validSrc = Join-Path $TmpDir "valid.mg"
    $loopSrc = Join-Path $TmpDir "loop.mg"

    @"
let x = 123
print(x)
"@ | Set-Content -LiteralPath $validSrc -Encoding Ascii

    @"
let total = 0
for i in 0..10
    total = total + i
end
print(total)
"@ | Set-Content -LiteralPath $loopSrc -Encoding Ascii

    foreach ($source in @($validSrc, $loopSrc)) {
        $build = Invoke-CapturedProcess -FilePath $Binary `
            -Arguments @("build", $source) -TimeoutSeconds $Timeout
        if (-not $build.Exited) { throw "Bytecode build timed out for $source" }
        if ($build.ExitCode -ne 0) {
            throw "Bytecode build failed for ${source}: $($build.Stderr)"
        }
    }

    $validMgc = Join-Path $TmpDir "valid.mgc"
    $loopMgc = Join-Path $TmpDir "loop.mgc"
    $mutDir = Join-Path $TmpDir "mutated"
    $manifest = Join-Path $TmpDir "manifest.txt"
    New-Item -ItemType Directory -Path $mutDir | Out-Null

    $validRun = Invoke-CapturedProcess -FilePath $Binary `
        -Arguments @($validMgc) -TimeoutSeconds $Timeout
    $validOutput = ([string]$validRun.Stdout).
        Replace("`r`n", "`n").Replace("`r", "`n").TrimEnd()
    if (-not $validRun.Exited) {
        throw "Valid bytecode execution timed out"
    }
    if ($validRun.ExitCode -ne 0 -or
            -not [string]::IsNullOrWhiteSpace($validRun.Stderr) -or
            $validOutput -cne "123") {
        throw "Valid bytecode execution failed: exit $($validRun.ExitCode), " +
            "stdout '$validOutput', stderr '$($validRun.Stderr)'"
    }

    $pyScript = @'
import pathlib, shutil, struct, sys

valid_path = pathlib.Path(sys.argv[1])
loop_path = pathlib.Path(sys.argv[2])
mut_dir = pathlib.Path(sys.argv[3])
manifest_path = pathlib.Path(sys.argv[4])

OP_LOADK = 0
JUMP_OPS = {48, 49, 52, 53, 54, 55}

def read_i32(buf, offset):
    if offset + 4 > len(buf): raise ValueError("truncated i32")
    return struct.unpack_from("<i", buf, offset)[0], offset + 4

def read_u32(buf, offset):
    if offset + 4 > len(buf): raise ValueError("truncated u32")
    return struct.unpack_from("<I", buf, offset)[0], offset + 4

def skip_string(buf, offset):
    length, offset = read_i32(buf, offset)
    if length == -1: return offset
    if length < 0: raise ValueError("bad string length")
    end = offset + length
    if end > len(buf): raise ValueError("truncated string")
    return end

def skip_value(buf, offset, meta):
    if offset >= len(buf): raise ValueError("truncated value")
    value_offset = offset
    value_type = buf[offset]
    meta["value_offsets"].append(value_offset)
    offset += 1
    if value_type == 0: return offset + 8
    if value_type == 1: return offset
    if value_type == 2: return offset + 1
    if value_type == 3: return skip_string(buf, offset)
    if value_type == 4:
        _, offset = parse_function(buf, offset)
        return offset
    raise ValueError("bad value type")

def parse_function(buf, offset):
    meta = {"reg_count_offset": offset + 8, "value_offsets": [], "loadk_offsets": [], "jump_offsets": []}
    offset += 12
    offset = skip_string(buf, offset)
    offset = skip_string(buf, offset)
    code_count_offset = offset
    code_count, offset = read_i32(buf, offset)
    if code_count < 0: raise ValueError("bad code count")
    code_offset = offset
    meta["code_count_offset"] = code_count_offset
    meta["code_offset"] = code_offset
    meta["code_count"] = code_count
    offset += code_count * 4
    offset += code_count * 4
    const_count, offset = read_i32(buf, offset)
    if const_count < 0: raise ValueError("bad const count")
    for _ in range(const_count):
        offset = skip_value(buf, offset, meta)
    for i in range(code_count):
        inst_offset = code_offset + i * 4
        inst, _ = read_u32(buf, inst_offset)
        op = inst & 0xFF
        if op == OP_LOADK: meta["loadk_offsets"].append(inst_offset)
        if op in JUMP_OPS: meta["jump_offsets"].append(inst_offset)
    return meta, offset

def parse_file(data):
    meta, end = parse_function(data, 4)
    meta["end"] = end
    return meta

def write_i32(buf, offset, value):
    struct.pack_into("<i", buf, offset, value)

def write_variant(name, source, mutate):
    data = bytearray(source)
    meta = parse_file(data)
    mutate(data, meta)
    out = mut_dir / f"{name}.mgc"
    out.write_bytes(data)
    return out

valid = valid_path.read_bytes()
loop = loop_path.read_bytes()
variants = []

def add(name, source, mutate):
    variants.append((name, write_variant(name, source, mutate)))

add("bad_magic", valid, lambda data, meta: data.__setitem__(slice(0, 4), b"\x00\x00\x00\x00"))
add("trailing_bytes", valid, lambda data, meta: data.extend(b"\x00"))
add("truncated_file", valid, lambda data, meta: data.__delitem__(slice(len(data) - 1, len(data))))
add("zero_register_count", valid, lambda data, meta: write_i32(data, meta["reg_count_offset"], 0))
add("too_many_registers", valid, lambda data, meta: write_i32(data, meta["reg_count_offset"], 257))
add("invalid_opcode", valid, lambda data, meta: data.__setitem__(meta["code_offset"], 255))
add("invalid_register", valid, lambda data, meta: data.__setitem__(meta["code_offset"] + 1, 255))
add("invalid_value_tag", valid, lambda data, meta: data.__setitem__(meta["value_offsets"][0], 255))

def invalid_constant(data, meta):
    if not meta["loadk_offsets"]: raise ValueError("no OP_LOADK")
    inst_offset = meta["loadk_offsets"][0]
    data[inst_offset + 2] = 0xFF
    data[inst_offset + 3] = 0xFF

def invalid_jump(data, meta):
    if not meta["jump_offsets"]: raise ValueError("no jump opcode")
    inst_offset = meta["jump_offsets"][0]
    op = data[inst_offset]
    data[inst_offset:inst_offset + 4] = bytes((op, 0xFF, 0xFF, 0xFF))

add("invalid_constant_index", valid, invalid_constant)
add("invalid_jump_target", loop, invalid_jump)

with manifest_path.open("w", encoding="utf-8") as m:
    for name, path in variants:
        m.write(f"{name}\t{path}\n")
'@

    $pyFile = Join-Path $TmpDir "mutate_bytecode.py"
    Set-Content -Path $pyFile -Value $pyScript -Encoding UTF8
    $python = Find-Python
    $pythonRun = Invoke-CapturedProcess -FilePath $python.Exe `
        -Arguments @($python.Prefix + @($pyFile, $validMgc, $loopMgc, $mutDir, $manifest)) `
        -TimeoutSeconds $Timeout
    if (-not $pythonRun.Exited) { throw "Python bytecode mutation timed out" }
    if ($pythonRun.ExitCode -ne 0) {
        throw "Python bytecode mutation failed: $($pythonRun.Stderr)"
    }

    $Passed = 0
    $Failed = 0

    Write-Host "Magnesium Bytecode Hardening Tests"
    Write-Host "----------------------------------------------"

    Get-Content $manifest | ForEach-Object {
        $parts = $_ -split "`t"
        $name = $parts[0]
        $path = $parts[1]

        $run = Invoke-CapturedProcess -FilePath $Binary -Arguments @($path) `
            -TimeoutSeconds $Timeout
        if (-not $run.Exited) {
            Write-Host "  FAIL  $name (timed out after ${Timeout}s)" -ForegroundColor Red
            $Failed++
            return
        }
        $output = [string]$run.Stdout + [string]$run.Stderr
        $exitCode = $run.ExitCode

        if ($exitCode -ge 128) {
            Write-Host "  FAIL  $name (terminated by signal)" -ForegroundColor Red
            $Failed++
        } elseif ($exitCode -eq 0) {
            Write-Host "  FAIL  $name (accepted malformed bytecode)" -ForegroundColor Red
            $Failed++
        } elseif ($output -match "Could not load bytecode") {
            Write-Host "  PASS  $name" -ForegroundColor Green
            $Passed++
        } else {
            Write-Host "  FAIL  $name (unexpected output)" -ForegroundColor Red
            $Failed++
        }
    }

    Write-Host "----------------------------------------------"
    $total = $Passed + $Failed
    Write-Host "Results: $total tests | $Passed passed | $Failed failed"

    if ($Failed -ne 0) { exit 1 }
} finally {
    Remove-Item -Recurse -Force $TmpDir
}
