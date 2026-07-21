#!/bin/bash
# Magnesium Test Runner
# Runs all .mg test files and validates output against expected results.
#
# Usage:
#   ./run_tests.sh            Run all tests
#   ./run_tests.sh -v         Verbose mode (show diff on failure)
#   ./run_tests.sh test_math  Run only tests matching the pattern

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$REPO_ROOT/magnesium"
TEST_DIR="$REPO_ROOT/tests"
EXPECTED_DIR="$REPO_ROOT/tests/expected"
TIMEOUT=${TIMEOUT:-10}

# Tests that are not standalone.
SKIP_TESTS=()

# Tests with non-deterministic output.
NONDETERMINISTIC_TESTS=()

VERBOSE=0
FILTER=""

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        *) FILTER="$arg" ;;
    esac
done

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

# Counters
PASSED=0
FAILED=0
SKIPPED=0
ERRORS=""

# Check binary exists
if [ ! -x "$BINARY" ]; then
    echo -e "${RED}Error: $BINARY not found or not executable.${RESET}"
    echo "Run 'make' first."
    exit 1
fi

echo -e "${BOLD}Magnesium Test Runner${RESET}"
echo "------------------------------------------------"

is_skipped() {
    local name="$1"
    for skip in "${SKIP_TESTS[@]}"; do
        if [ "$name" = "$skip" ]; then
            return 0
        fi
    done
    return 1
}

is_nondeterministic() {
    local name="$1"
    for nd in "${NONDETERMINISTIC_TESTS[@]}"; do
        if [ "$name" = "$nd" ]; then
            return 0
        fi
    done
    return 1
}

for test_file in "$TEST_DIR"/test_*.mg; do
    test_name=$(basename "$test_file" .mg)

    # Apply filter if provided
    if [ -n "$FILTER" ] && [[ "$test_name" != *"$FILTER"* ]]; then
        continue
    fi

    # Skip module files
    if is_skipped "$test_name"; then
        echo -e "  ${YELLOW}SKIP${RESET}  $test_name (module, not standalone)"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # Non-deterministic tests: just check they run without error
    if is_nondeterministic "$test_name"; then
        if timeout "$TIMEOUT" "$BINARY" "$test_file" > /dev/null 2>&1; then
            echo -e "  ${GREEN}PASS${RESET}  $test_name (non-deterministic, exit code only)"
            PASSED=$((PASSED + 1))
        else
            EXIT_CODE=$?
            if [ "$EXIT_CODE" -eq 124 ]; then
                echo -e "  ${RED}FAIL${RESET}  $test_name (timed out after ${TIMEOUT}s)"
            else
                echo -e "  ${RED}FAIL${RESET}  $test_name (exit code $EXIT_CODE)"
            fi
            FAILED=$((FAILED + 1))
            ERRORS="$ERRORS\n  - $test_name: non-zero exit or timeout"
        fi
        continue
    fi

    expected_file="$EXPECTED_DIR/${test_name}.expected"

    # Check that expected output file exists
    if [ ! -f "$expected_file" ]; then
        echo -e "  ${YELLOW}SKIP${RESET}  $test_name (no expected output file)"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # Run the test and capture combined stdout+stderr. Many negative tests
    # intentionally exit non-zero, so output remains the primary assertion.
    # Use a repo-root-relative path so error messages and imports match expected output.
    relative_test_file="tests/$(basename "$test_file")"
    set +e
    actual=$(cd "$REPO_ROOT" && timeout "$TIMEOUT" ./magnesium "$relative_test_file" 2>&1)
    EXIT_CODE=$?
    set -e
    actual=${actual//$'\r'/}

    # Check for timeout
    if [ "$EXIT_CODE" = "124" ]; then
        echo -e "  ${RED}FAIL${RESET}  $test_name (timed out after ${TIMEOUT}s)"
        FAILED=$((FAILED + 1))
        ERRORS="$ERRORS\n  - $test_name: timed out"
        continue
    fi

    if [ "$EXIT_CODE" -ge 128 ]; then
        SIGNAL=$((EXIT_CODE - 128))
        echo -e "  ${RED}FAIL${RESET}  $test_name (terminated by signal $SIGNAL)"
        FAILED=$((FAILED + 1))
        ERRORS="$ERRORS\n  - $test_name: terminated by signal $SIGNAL"

        if [ "$VERBOSE" -eq 1 ] && [ -n "$actual" ]; then
            echo ""
            echo -e "    ${CYAN}Output:${RESET}"
            echo "$actual" | sed 's/^/    | /'
            echo ""
        fi
        continue
    fi

    expected=$(tr -d '\r' < "$expected_file")

    if [ "$actual" = "$expected" ]; then
        echo -e "  ${GREEN}PASS${RESET}  $test_name"
        PASSED=$((PASSED + 1))
    else
        echo -e "  ${RED}FAIL${RESET}  $test_name"
        FAILED=$((FAILED + 1))
        ERRORS="$ERRORS\n  - $test_name: output mismatch"

        if [ "$VERBOSE" -eq 1 ]; then
            echo ""
            echo -e "    ${CYAN}Expected:${RESET}"
            echo "$expected" | sed 's/^/    | /'
            echo -e "    ${CYAN}Actual:${RESET}"
            echo "$actual" | sed 's/^/    | /'
            echo -e "    ${CYAN}Diff:${RESET}"
            diff <(echo "$expected") <(echo "$actual") | sed 's/^/    /' || true
            echo ""
        fi
    fi
done

# Summary
echo "------------------------------------------------"
TOTAL=$((PASSED + FAILED + SKIPPED))
echo -e "${BOLD}Results:${RESET} $TOTAL tests | ${GREEN}$PASSED passed${RESET} | ${RED}$FAILED failed${RESET} | ${YELLOW}$SKIPPED skipped${RESET}"

if [ "$FAILED" -gt 0 ]; then
    echo -e "\n${RED}${BOLD}Failures:${RESET}${ERRORS}"
    echo ""
    exit 1
else
    echo ""
    exit 0
fi
