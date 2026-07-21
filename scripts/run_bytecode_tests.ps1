$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Binary = if ($env:BINARY) { $env:BINARY } else { Join-Path $RepoRoot "magnesium.exe" }
$Timeout = if ($env:TIMEOUT) { [int]$env:TIMEOUT } else { 5 }

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
"@ | Set-Content $validSrc

    @"
let total = 0
for i in 0..10
    total = total + i
end
print(total)
"@ | Set-Content $loopSrc

    & $Binary build $validSrc | Out-Null
    & $Binary build $loopSrc | Out-Null

    $validMgc = Join-Path $TmpDir "valid.mgc"
    $loopMgc = Join-Path $TmpDir "loop.mgc"
    $mutDir = Join-Path $TmpDir "mutated"
    $manifest = Join-Path $TmpDir "manifest.txt"
    New-Item -ItemType Directory -Path $mutDir | Out-Null

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
    & python3 $pyFile $validMgc $loopMgc $mutDir $manifest

    $Passed = 0
    $Failed = 0

    Write-Host "Magnesium Bytecode Hardening Tests"
    Write-Host "----------------------------------------------"

    Get-Content $manifest | ForEach-Object {
        $parts = $_ -split "`t"
        $name = $parts[0]
        $path = $parts[1]

        $outFile = Join-Path $TmpDir "$name.out"
        $errFile = Join-Path $TmpDir "$name.err"
        $proc = Start-Process -FilePath $Binary -ArgumentList @($path) -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        $stdout = if (Test-Path $outFile) { Get-Content $outFile -Raw } else { "" }
        $stderr = if (Test-Path $errFile) { Get-Content $errFile -Raw } else { "" }
        $output = $stdout + $stderr
        $exitCode = $proc.ExitCode

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
