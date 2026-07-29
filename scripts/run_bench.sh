#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_DIR="$REPO_ROOT/benchmark"

die() {
    printf 'Benchmark gate failed: %s\n' "$*" >&2
    exit 1
}

read_integer_setting() {
    local name="$1"
    local default_value="$2"
    local minimum="$3"
    local value="${!name:-$default_value}"

    [[ "$value" =~ ^[0-9]+$ ]] ||
        die "$name must be an integer greater than or equal to $minimum."
    (( value >= minimum )) ||
        die "$name must be an integer greater than or equal to $minimum."
    printf '%s' "$value"
}

read_bool_setting() {
    local value="${!1:-${2:-false}}"
    case "${value,,}" in
        1|true|yes|on) return 0 ;;
        0|false|no|off) return 1 ;;
        *) die "$1 must be a boolean setting (0/1 or false/true)." ;;
    esac
}

resolve_executable() {
    local candidate="$1"
    if [[ "$candidate" == */* ]]; then
        [[ -x "$candidate" ]] || return 1
        printf '%s' "$candidate"
        return 0
    fi
    command -v -- "$candidate" 2>/dev/null
}

find_runtime() {
    local override="$1"
    local version_pattern="$2"
    shift 2
    local candidate executable output status

    if [[ -n "$override" ]]; then
        set -- "$override"
    fi

    for candidate in "$@"; do
        executable="$(resolve_executable "$candidate")" || continue
        if output="$("$executable" "${VERSION_ARGS[@]}" 2>&1)"; then
            status=0
        else
            status=$?
        fi
        if (( status == 0 )) &&
                grep -Eq -- "$version_pattern" <<<"$output"; then
            FOUND_EXECUTABLE="$executable"
            FOUND_VERSION="$(tr '\r\n' '  ' <<<"$output" |
                awk '{$1=$1; print}')"
            return 0
        fi
        if [[ -n "$override" ]]; then
            die "Runtime override '$override' has unsupported version output: $output"
        fi
    done
    return 1
}

format_command() {
    local argument
    for argument in "$@"; do
        printf '%q ' "$argument"
    done
}

now_nanoseconds() {
    local value
    value="$(date +%s%N)"
    [[ "$value" =~ ^[0-9]+$ ]] ||
        die "This benchmark runner requires a date implementation with %N support."
    printf '%s' "$value"
}

run_capture() {
    local timeout_seconds="$1"
    shift
    local start end status

    start="$(now_nanoseconds)"
    # GNU timeout waits roughly 100 ms after short-lived children on some WSL
    # releases, which completely distorts small benchmarks. Perl installs the
    # same process-level alarm and then execs the runtime, so no watchdog
    # process or polling delay is included in the measurement.
    if perl -e 'alarm shift; exec @ARGV or die "exec failed: $!\n"' \
            "$timeout_seconds" "$@" >"$CAPTURE_STDOUT" 2>"$CAPTURE_STDERR"; then
        status=0
    else
        status=$?
    fi
    end="$(now_nanoseconds)"

    RUN_STATUS="$status"
    RUN_SECONDS="$(awk -v start="$start" -v end="$end" \
        'BEGIN { printf "%.9f", (end - start) / 1000000000 }')"
}

assert_successful_run() {
    local label="$1"
    shift

    if (( RUN_STATUS == 142 || RUN_STATUS == 137 )); then
        die "$label timed out after ${BENCH_TIMEOUT}s: $(format_command "$@")"
    fi
    if (( RUN_STATUS != 0 )); then
        printf '%s\n' "Command stderr:" >&2
        sed 's/^/  /' "$CAPTURE_STDERR" >&2
        die "$label exited with status $RUN_STATUS: $(format_command "$@")"
    fi
    if [[ -s "$CAPTURE_STDERR" ]]; then
        printf '%s\n' "Unexpected command stderr:" >&2
        sed 's/^/  /' "$CAPTURE_STDERR" >&2
        die "$label wrote to stderr: $(format_command "$@")"
    fi
}

outputs_equal() {
    local expected_file="$1"
    local actual_file="$2"

    cmp -s -- "$expected_file" "$actual_file" && return 0
    awk '
        function abs(value) {
            return value < 0 ? -value : value
        }
        function numeric(value) {
            return value ~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/
        }
        NR == FNR {
            sub(/\r$/, "")
            expected[++expected_count] = $0
            next
        }
        {
            sub(/\r$/, "")
            actual[++actual_count] = $0
        }
        END {
            if (expected_count != actual_count) {
                exit 1
            }
            for (line = 1; line <= expected_count; line++) {
                if (expected[line] == actual[line]) {
                    continue
                }
                expected_parts = split(expected[line], expected_tokens, /[[:space:]]+/)
                actual_parts = split(actual[line], actual_tokens, /[[:space:]]+/)
                if (expected_parts != actual_parts) {
                    exit 1
                }
                for (part = 1; part <= expected_parts; part++) {
                    if (expected_tokens[part] == actual_tokens[part]) {
                        continue
                    }
                    if (!numeric(expected_tokens[part]) ||
                            !numeric(actual_tokens[part])) {
                        exit 1
                    }
                    # Accept representation differences such as 1, 1.0, and
                    # 1e0, but require the decoded numbers to be identical.
                    if ((expected_tokens[part] + 0) != (actual_tokens[part] + 0)) {
                        exit 1
                    }
                }
            }
        }
    ' "$expected_file" "$actual_file"
}

run_build_step() {
    local label="$1"
    shift

    printf '  %-24s ' "$label"
    format_command "$@"
    printf '\n'
    run_capture "$BENCH_TIMEOUT" "$@"
    if (( RUN_STATUS != 0 )); then
        sed 's/^/  /' "$CAPTURE_STDERR" >&2
        die "$label failed with status $RUN_STATUS."
    fi
}

validate_command() {
    local label="$1"
    local expected_file="$2"
    shift 2

    printf '  check %-20s ' "$label"
    format_command "$@"
    printf '\n'
    run_capture "$BENCH_TIMEOUT" "$@"
    assert_successful_run "$label" "$@"
    if ! outputs_equal "$expected_file" "$CAPTURE_STDOUT"; then
        printf '%s\n' "Expected output:" >&2
        sed 's/^/  /' "$expected_file" >&2
        printf '%s\n' "Actual output:" >&2
        sed 's/^/  /' "$CAPTURE_STDOUT" >&2
        die "Output mismatch for $label."
    fi
}

measure_command() {
    local label="$1"
    local expected_file="$2"
    shift 2
    local run_index
    local -a measurements=()

    for ((run_index = 0; run_index < WARMUPS; run_index++)); do
        run_capture "$BENCH_TIMEOUT" "$@"
        assert_successful_run "$label" "$@"
        outputs_equal "$expected_file" "$CAPTURE_STDOUT" ||
            die "Output changed during warmup for $label."
    done

    for ((run_index = 0; run_index < RUNS; run_index++)); do
        run_capture "$BENCH_TIMEOUT" "$@"
        assert_successful_run "$label" "$@"
        outputs_equal "$expected_file" "$CAPTURE_STDOUT" ||
            die "Output changed during timed run for $label."
        measurements+=("$RUN_SECONDS")
    done

    MEASURE_TIMES="$(IFS=', '; printf '%s' "${measurements[*]}")"
    MEASURE_AVERAGE="$(printf '%s\n' "${measurements[@]}" |
        awk '{ total += $1 } END { printf "%.6f", total / NR }')"
    mapfile -t SORTED_MEASUREMENTS < <(
        printf '%s\n' "${measurements[@]}" | sort -n
    )
    local count="${#SORTED_MEASUREMENTS[@]}"
    local middle=$((count / 2))
    if (( count % 2 == 1 )); then
        MEASURE_MEDIAN="$(printf '%.6f' "${SORTED_MEASUREMENTS[$middle]}")"
    else
        MEASURE_MEDIAN="$(awk \
            -v left="${SORTED_MEASUREMENTS[$((middle - 1))]}" \
            -v right="${SORTED_MEASUREMENTS[$middle]}" \
            'BEGIN { printf "%.6f", (left + right) / 2 }')"
    fi
}

write_result() {
    local label="$1"
    local relative
    relative="$(awk -v runtime="$MEASURE_MEDIAN" \
        -v magnesium="$MAGNESIUM_MEDIAN" \
        'BEGIN { printf "%.2f", runtime / magnesium }')"
    printf '%-23s %10ss %10ss %8sx  %s\n' \
        "$label" "$MEASURE_AVERAGE" "$MEASURE_MEDIAN" "$relative" \
        "$MEASURE_TIMES"
}

check_performance() {
    local benchmark="$1"
    local mode="$2"
    local runtime="$3"
    local runtime_median="$4"
    local relative

    if awk -v magnesium="$MAGNESIUM_MEDIAN" -v reference="$runtime_median" \
            'BEGIN { exit !(magnesium >= reference) }'; then
        relative="$(awk -v runtime="$runtime_median" \
            -v magnesium="$MAGNESIUM_MEDIAN" \
            'BEGIN { printf "%.2f", runtime / magnesium }')"
        PERFORMANCE_FAILURES+=(
            "$benchmark ($mode): Magnesium ${MAGNESIUM_MEDIAN}s did not beat $runtime ${runtime_median}s (${relative}x; must be >1.00x)"
        )
    fi
}

RUNS="$(read_integer_setting RUNS 5 1)"
WARMUPS="$(read_integer_setting WARMUPS 1 0)"
BENCH_TIMEOUT="$(read_integer_setting BENCH_TIMEOUT 30 1)"
BENCH_FILTER="${BENCH:-}"
if read_bool_setting REQUIRE_ALL_RUNTIMES true; then
    REQUIRE_ALL_RUNTIMES_ENABLED=true
else
    REQUIRE_ALL_RUNTIMES_ENABLED=false
fi
if read_bool_setting PERFORMANCE_GATE true; then
    PERFORMANCE_GATE_ENABLED=true
else
    PERFORMANCE_GATE_ENABLED=false
fi

BENCHMARKS=(
    fib35 binary_trees sieve mandelbrot dict_bench arith_loop call_loop
    closure_loop array_loop object_fields gc_alloc string_concat control_flow
    fallible_lookup native_len coroutine_switch
)

if [[ -n "$BENCH_FILTER" ]]; then
    found=false
    for benchmark in "${BENCHMARKS[@]}"; do
        if [[ "$benchmark" == "$BENCH_FILTER" ]]; then
            found=true
            break
        fi
    done
    "$found" || die "Unknown benchmark '$BENCH_FILTER'. Available: ${BENCHMARKS[*]}"
    BENCHMARKS=("$BENCH_FILTER")
fi

command -v perl >/dev/null ||
    die "Perl is required for low-overhead benchmark timeouts."

MAGNESIUM_OVERRIDE="${BINARY:-${MAGNESIUM_BIN:-}}"
if [[ -z "$MAGNESIUM_OVERRIDE" && -x "$REPO_ROOT/magnesium" ]]; then
    MAGNESIUM_OVERRIDE="$REPO_ROOT/magnesium"
fi
VERSION_ARGS=(--version)
FOUND_EXECUTABLE=
FOUND_VERSION=
find_runtime "$MAGNESIUM_OVERRIDE" '^Magnesium v' magnesium ||
    die "Magnesium was not found. Set BINARY or MAGNESIUM_BIN."
MAGNESIUM_EXECUTABLE="$FOUND_EXECUTABLE"
MAGNESIUM_VERSION="$FOUND_VERSION"

VERSION_ARGS=(-v)
FOUND_EXECUTABLE=
FOUND_VERSION=
if find_runtime "${LUA:-}" 'Lua 5[.]4' lua5.4 lua54 lua; then
    HAVE_LUA=true
    LUA_EXECUTABLE="$FOUND_EXECUTABLE"
    LUA_VERSION="$FOUND_VERSION"
else
    HAVE_LUA=false
fi

FOUND_EXECUTABLE=
FOUND_VERSION=
if find_runtime "${LUAC:-}" 'Lua 5[.]4' luac5.4 luac54 luac; then
    HAVE_LUAC=true
    LUAC_EXECUTABLE="$FOUND_EXECUTABLE"
    LUAC_VERSION="$FOUND_VERSION"
else
    HAVE_LUAC=false
fi

VERSION_ARGS=(--version)
FOUND_EXECUTABLE=
FOUND_VERSION=
if find_runtime "${PYTHON:-}" '^Python 3[.]' python3 python; then
    HAVE_PYTHON=true
    PYTHON_EXECUTABLE="$FOUND_EXECUTABLE"
    PYTHON_VERSION="$FOUND_VERSION"
else
    HAVE_PYTHON=false
fi

printf '%s\n' "=== Benchmark release gate ==="
printf 'Runs: %s; warmups: %s; timeout: %ss\n' \
    "$RUNS" "$WARMUPS" "$BENCH_TIMEOUT"
printf 'Performance gate: %s; require all runtimes: %s\n' \
    "$PERFORMANCE_GATE_ENABLED" "$REQUIRE_ALL_RUNTIMES_ENABLED"
printf 'Magnesium: %s (%s)\n' "$MAGNESIUM_EXECUTABLE" "$MAGNESIUM_VERSION"
if "$HAVE_LUA"; then
    printf 'Lua:       %s (%s)\n' "$LUA_EXECUTABLE" "$LUA_VERSION"
else
    printf '%s\n' "Lua:       unavailable (requires Lua 5.4; set LUA)"
fi
if "$HAVE_LUAC"; then
    printf 'Lua bytecode compiler: %s (%s)\n' \
        "$LUAC_EXECUTABLE" "$LUAC_VERSION"
else
    printf '%s\n' "Lua bytecode compiler: unavailable (set LUAC)"
fi
if "$HAVE_PYTHON"; then
    printf 'Python:    %s (%s)\n' "$PYTHON_EXECUTABLE" "$PYTHON_VERSION"
else
    printf '%s\n' "Python:    unavailable (requires Python 3; set PYTHON)"
fi

if "$REQUIRE_ALL_RUNTIMES_ENABLED" &&
        { ! "$HAVE_LUA" || ! "$HAVE_LUAC" || ! "$HAVE_PYTHON"; }; then
    die "REQUIRE_ALL_RUNTIMES is enabled, but Lua 5.4, luac 5.4, and Python 3 are not all available."
fi

ARTIFACT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/magnesium-bench.XXXXXX")"
CAPTURE_STDOUT="$ARTIFACT_DIR/capture.stdout"
CAPTURE_STDERR="$ARTIFACT_DIR/capture.stderr"
BENCHMARK_MAGNESIUM="$ARTIFACT_DIR/magnesium"
cp -- "$MAGNESIUM_EXECUTABLE" "$BENCHMARK_MAGNESIUM"
chmod 755 "$BENCHMARK_MAGNESIUM"

cleanup() {
    case "$ARTIFACT_DIR" in
        "${TMPDIR:-/tmp}"/magnesium-bench.*)
            rm -rf -- "$ARTIFACT_DIR"
            ;;
        *)
            printf 'Refusing to remove unexpected artifact path: %s\n' \
                "$ARTIFACT_DIR" >&2
            ;;
    esac
}
trap cleanup EXIT

printf '\n%s\n' "=== Preparation ==="
for benchmark in "${BENCHMARKS[@]}"; do
    printf '%s\n' "$benchmark"
    mg_source="$BENCH_DIR/$benchmark.mg"
    lua_source="$BENCH_DIR/$benchmark.lua"
    python_source="$BENCH_DIR/$benchmark.py"
    [[ -f "$mg_source" ]] || die "Missing benchmark source: $mg_source"
    [[ -f "$lua_source" ]] || die "Missing benchmark source: $lua_source"
    [[ -f "$python_source" ]] || die "Missing benchmark source: $python_source"

    cp -- "$mg_source" "$ARTIFACT_DIR/$benchmark.mg"
    cp -- "$lua_source" "$ARTIFACT_DIR/$benchmark.lua"
    cp -- "$python_source" "$ARTIFACT_DIR/$benchmark.py"
    run_build_step "Magnesium bytecode" "$BENCHMARK_MAGNESIUM" \
        build "$ARTIFACT_DIR/$benchmark.mg"
    [[ -f "$ARTIFACT_DIR/$benchmark.mgc" ]] ||
        die "Magnesium build did not create $ARTIFACT_DIR/$benchmark.mgc"

    if "$HAVE_LUA" && "$HAVE_LUAC"; then
        run_build_step "Lua bytecode" "$LUAC_EXECUTABLE" \
            -o "$ARTIFACT_DIR/$benchmark.luac" \
            "$ARTIFACT_DIR/$benchmark.lua"
    fi
    if "$HAVE_PYTHON"; then
        python_compile_code='import py_compile,sys;py_compile.compile(sys.argv[1],cfile=sys.argv[2],doraise=True)'
        run_build_step "Python bytecode" "$PYTHON_EXECUTABLE" \
            -c "$python_compile_code" "$ARTIFACT_DIR/$benchmark.py" \
            "$ARTIFACT_DIR/$benchmark.pyc"
    fi
done

printf '\n%s\n' "=== Correctness preflight ==="
for benchmark in "${BENCHMARKS[@]}"; do
    printf '%s\n' "$benchmark"
    expected_file="$ARTIFACT_DIR/$benchmark.expected"

    printf '  baseline %-17s ' "Magnesium"
    format_command "$BENCHMARK_MAGNESIUM" "$ARTIFACT_DIR/$benchmark.mg"
    printf '\n'
    run_capture "$BENCH_TIMEOUT" "$BENCHMARK_MAGNESIUM" \
        "$ARTIFACT_DIR/$benchmark.mg"
    assert_successful_run "Magnesium" "$BENCHMARK_MAGNESIUM" \
        "$ARTIFACT_DIR/$benchmark.mg"
    cp -- "$CAPTURE_STDOUT" "$expected_file"

    validate_command "Magnesium" "$expected_file" "$BENCHMARK_MAGNESIUM" \
        "$ARTIFACT_DIR/$benchmark.mg"
    if "$HAVE_LUA"; then
        validate_command "Lua 5.4" "$expected_file" "$LUA_EXECUTABLE" \
            "$ARTIFACT_DIR/$benchmark.lua"
    fi
    if "$HAVE_PYTHON"; then
        validate_command "Python 3" "$expected_file" "$PYTHON_EXECUTABLE" \
            "$ARTIFACT_DIR/$benchmark.py"
    fi

    validate_command "Magnesium (MGC)" "$expected_file" \
        "$BENCHMARK_MAGNESIUM" "$ARTIFACT_DIR/$benchmark.mgc"
    if "$HAVE_LUA" && "$HAVE_LUAC"; then
        validate_command "Lua 5.4 (LUAC)" "$expected_file" \
            "$LUA_EXECUTABLE" "$ARTIFACT_DIR/$benchmark.luac"
    fi
    if "$HAVE_PYTHON"; then
        validate_command "Python 3 (PYC)" "$expected_file" \
            "$PYTHON_EXECUTABLE" "$ARTIFACT_DIR/$benchmark.pyc"
    fi
done

printf '\n%s\n' "=== Timed benchmarks ==="
PERFORMANCE_FAILURES=()
for benchmark in "${BENCHMARKS[@]}"; do
    printf '\n%s\n%s\n' "--------------------------------------------------------" "$benchmark"
    expected_file="$ARTIFACT_DIR/$benchmark.expected"

    printf '\n%s - source mode\n' "$benchmark"
    printf '%-23s %11s %11s %9s  %s\n' \
        "Runtime" "Average" "Median" "vs Mg" "Runs"
    measure_command "Magnesium" "$expected_file" "$BENCHMARK_MAGNESIUM" \
        "$ARTIFACT_DIR/$benchmark.mg"
    MAGNESIUM_MEDIAN="$MEASURE_MEDIAN"
    write_result "Magnesium"
    if "$HAVE_LUA"; then
        measure_command "Lua 5.4" "$expected_file" "$LUA_EXECUTABLE" \
            "$ARTIFACT_DIR/$benchmark.lua"
        write_result "Lua 5.4"
        if "$PERFORMANCE_GATE_ENABLED"; then
            check_performance "$benchmark" "source" "Lua 5.4" \
                "$MEASURE_MEDIAN"
        fi
    fi
    if "$HAVE_PYTHON"; then
        measure_command "Python 3" "$expected_file" "$PYTHON_EXECUTABLE" \
            "$ARTIFACT_DIR/$benchmark.py"
        write_result "Python 3"
        if "$PERFORMANCE_GATE_ENABLED"; then
            check_performance "$benchmark" "source" "Python 3" \
                "$MEASURE_MEDIAN"
        fi
    fi

    printf '\n%s - bytecode mode\n' "$benchmark"
    printf '%-23s %11s %11s %9s  %s\n' \
        "Runtime" "Average" "Median" "vs Mg" "Runs"
    measure_command "Magnesium (MGC)" "$expected_file" \
        "$BENCHMARK_MAGNESIUM" "$ARTIFACT_DIR/$benchmark.mgc"
    MAGNESIUM_MEDIAN="$MEASURE_MEDIAN"
    write_result "Magnesium (MGC)"
    if "$HAVE_LUA" && "$HAVE_LUAC"; then
        measure_command "Lua 5.4 (LUAC)" "$expected_file" \
            "$LUA_EXECUTABLE" "$ARTIFACT_DIR/$benchmark.luac"
        write_result "Lua 5.4 (LUAC)"
        if "$PERFORMANCE_GATE_ENABLED"; then
            check_performance "$benchmark" "bytecode" "Lua 5.4 (LUAC)" \
                "$MEASURE_MEDIAN"
        fi
    fi
    if "$HAVE_PYTHON"; then
        measure_command "Python 3 (PYC)" "$expected_file" \
            "$PYTHON_EXECUTABLE" "$ARTIFACT_DIR/$benchmark.pyc"
        write_result "Python 3 (PYC)"
        if "$PERFORMANCE_GATE_ENABLED"; then
            check_performance "$benchmark" "bytecode" "Python 3 (PYC)" \
                "$MEASURE_MEDIAN"
        fi
    fi
done

if "$PERFORMANCE_GATE_ENABLED" &&
        ((${#PERFORMANCE_FAILURES[@]} > 0)); then
    printf '\n%s\n' "=== Performance gate failures ===" >&2
    for failure in "${PERFORMANCE_FAILURES[@]}"; do
        printf '  - %s\n' "$failure" >&2
    done
    die "${#PERFORMANCE_FAILURES[@]} runtime comparisons failed."
fi

printf '\n%s\n' "Benchmark release gate passed."
