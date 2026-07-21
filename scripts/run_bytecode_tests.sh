#!/bin/bash
# Magnesium bytecode hardening tests.
# Builds valid .mgc files, mutates precise binary fields, and verifies the
# loader rejects malformed bytecode without running, hanging, or crashing.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY=${BINARY:-$REPO_ROOT/magnesium}
TIMEOUT=${TIMEOUT:-5}
TMP_DIR=$(mktemp -d)

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable."
    exit 1
fi

VALID_SRC="$TMP_DIR/valid.mg"
LOOP_SRC="$TMP_DIR/loop.mg"
VALID_MGC="${VALID_SRC%.mg}.mgc"
LOOP_MGC="${LOOP_SRC%.mg}.mgc"
MUT_DIR="$TMP_DIR/mutated"
MANIFEST="$TMP_DIR/manifest.txt"
mkdir -p "$MUT_DIR"

cat > "$VALID_SRC" <<'MG'
let x = 123
print(x)
MG

cat > "$LOOP_SRC" <<'MG'
let total = 0
for i in 0..10
    total = total + i
end
print(total)
MG

"$BINARY" build "$VALID_SRC" >/dev/null
"$BINARY" build "$LOOP_SRC" >/dev/null

expected="123"
actual=$(timeout "$TIMEOUT" "$BINARY" "$VALID_MGC" 2>&1)
if [ "$actual" != "$expected" ]; then
    echo "FAIL valid bytecode"
    echo "Expected: $expected"
    echo "Actual: $actual"
    exit 1
fi

python3 - "$VALID_MGC" "$LOOP_MGC" "$MUT_DIR" "$MANIFEST" <<'PY'
import pathlib
import shutil
import struct
import sys

valid_path = pathlib.Path(sys.argv[1])
loop_path = pathlib.Path(sys.argv[2])
mut_dir = pathlib.Path(sys.argv[3])
manifest_path = pathlib.Path(sys.argv[4])

OP_LOADK = 0
JUMP_OPS = {48, 49, 52, 53, 54, 55}


def read_i32(buf, offset):
    if offset + 4 > len(buf):
        raise ValueError("truncated i32")
    return struct.unpack_from("<i", buf, offset)[0], offset + 4


def read_u32(buf, offset):
    if offset + 4 > len(buf):
        raise ValueError("truncated u32")
    return struct.unpack_from("<I", buf, offset)[0], offset + 4


def skip_string(buf, offset):
    length, offset = read_i32(buf, offset)
    if length == -1:
        return offset
    if length < 0:
        raise ValueError("bad string length")
    end = offset + length
    if end > len(buf):
        raise ValueError("truncated string")
    return end


def skip_value(buf, offset, meta):
    if offset >= len(buf):
        raise ValueError("truncated value")
    value_offset = offset
    value_type = buf[offset]
    meta["value_offsets"].append(value_offset)
    offset += 1
    if value_type == 0:
        return offset + 8
    if value_type == 1:
        return offset
    if value_type == 2:
        return offset + 1
    if value_type == 3:
        return skip_string(buf, offset)
    if value_type == 4:
        _, offset = parse_function(buf, offset)
        return offset
    raise ValueError("bad value type")


def parse_function(buf, offset):
    meta = {
        "reg_count_offset": offset + 8,
        "value_offsets": [],
        "loadk_offsets": [],
        "jump_offsets": [],
    }
    offset += 12
    offset = skip_string(buf, offset)
    offset = skip_string(buf, offset)
    code_count_offset = offset
    code_count, offset = read_i32(buf, offset)
    if code_count < 0:
        raise ValueError("bad code count")

    code_offset = offset
    meta["code_count_offset"] = code_count_offset
    meta["code_offset"] = code_offset
    meta["code_count"] = code_count
    offset += code_count * 4
    offset += code_count * 4

    const_count, offset = read_i32(buf, offset)
    if const_count < 0:
        raise ValueError("bad const count")
    for _ in range(const_count):
        offset = skip_value(buf, offset, meta)

    for i in range(code_count):
        inst_offset = code_offset + i * 4
        inst, _ = read_u32(buf, inst_offset)
        op = inst & 0xFF
        if op == OP_LOADK:
            meta["loadk_offsets"].append(inst_offset)
        if op in JUMP_OPS:
            meta["jump_offsets"].append(inst_offset)

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
    if not meta["loadk_offsets"]:
        raise ValueError("test fixture has no OP_LOADK")
    inst_offset = meta["loadk_offsets"][0]
    data[inst_offset + 2] = 0xFF
    data[inst_offset + 3] = 0xFF


def invalid_jump(data, meta):
    if not meta["jump_offsets"]:
        raise ValueError("test fixture has no jump opcode")
    inst_offset = meta["jump_offsets"][0]
    op = data[inst_offset]
    data[inst_offset:inst_offset + 4] = bytes((op, 0xFF, 0xFF, 0xFF))


add("invalid_constant_index", valid, invalid_constant)
add("invalid_jump_target", loop, invalid_jump)

with manifest_path.open("w", encoding="utf-8") as manifest:
    for name, path in variants:
        manifest.write(f"{name}\t{path}\n")
PY

PASSED=0
FAILED=0

echo "Magnesium Bytecode Hardening Tests"
echo "------------------------------------------------"

while IFS=$'\t' read -r name path; do
    set +e
    output=$(timeout "$TIMEOUT" "$BINARY" "$path" 2>&1)
    status=$?
    set -e

    if [ "$status" -eq 124 ]; then
        echo "  FAIL  $name (timed out)"
        FAILED=$((FAILED + 1))
        continue
    fi

    if [ "$status" -ge 128 ]; then
        echo "  FAIL  $name (terminated by signal $((status - 128)))"
        if [ -n "$output" ]; then
            echo "$output" | sed 's/^/        /'
        fi
        FAILED=$((FAILED + 1))
        continue
    fi

    if [ "$status" -eq 0 ]; then
        echo "  FAIL  $name (accepted malformed bytecode)"
        FAILED=$((FAILED + 1))
        continue
    fi

    case "$output" in
        *"Could not load bytecode"*)
            echo "  PASS  $name"
            PASSED=$((PASSED + 1))
            ;;
        *)
            echo "  FAIL  $name (unexpected output)"
            echo "$output" | sed 's/^/        /'
            FAILED=$((FAILED + 1))
            ;;
    esac
done < "$MANIFEST"

echo "------------------------------------------------"
echo "Results: $((PASSED + FAILED)) tests | $PASSED passed | $FAILED failed"

if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
