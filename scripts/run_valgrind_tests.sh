#!/usr/bin/env bash
#
# Run unit tests under valgrind and save results.
#
# Usage:
#   ./scripts/run_valgrind_tests.sh [build_dir] [output_dir]
#
#   build_dir  — CMake build directory (default: ./build)
#   output_dir — directory to store valgrind reports (default: ./valgrind-reports)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SUPP_FILE="$SCRIPT_DIR/grpc_protobuf.supp"

BUILD_DIR="${1:-build}"
OUTPUT_DIR="${2:-valgrind-reports}"

# All unit test targets defined in test/CMakeLists.txt
TESTS=(
    test_limiter
    test_sampling
    test_cache
    test_http
    test_annotation
    test_callstack
    test_config
    test_sql
    test_stat
    test_url_stat
    test_span_event
    test_span
    test_noop
    test_grpc
    test_grpc_with_mocks
    test_agent_with_mocks
    test_utility
    test_logging
)

# Resolve to absolute paths
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

if ! command -v valgrind &>/dev/null; then
    echo "ERROR: valgrind is not installed." >&2
    exit 1
fi

echo "Build dir : $BUILD_DIR"
echo "Output dir: $OUTPUT_DIR"
echo "valgrind  : $(valgrind --version)"
echo "========================================"

PASS=0
FAIL=0
SKIP=0
SUMMARY=""

for test in "${TESTS[@]}"; do
    binary="$BUILD_DIR/test/$test"
    report="$OUTPUT_DIR/${test}.valgrind.txt"

    if [[ ! -x "$binary" ]]; then
        echo "[SKIP] $test — binary not found"
        SKIP=$((SKIP + 1))
        SUMMARY+="  [SKIP] $test\n"
        continue
    fi

    echo -n "[RUN]  $test ... "

    SUPP_OPT=()
    [[ -f "$SUPP_FILE" ]] && SUPP_OPT=(--suppressions="$SUPP_FILE")

    if valgrind \
        --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --error-exitcode=1 \
        "${SUPP_OPT[@]}" \
        --log-file="$report" \
        "$binary" &>/dev/null; then
        echo "PASS"
        PASS=$((PASS + 1))
        SUMMARY+="  [PASS] $test\n"
    else
        echo "FAIL  (see $report)"
        FAIL=$((FAIL + 1))
        SUMMARY+="  [FAIL] $test\n"
    fi
done

# Write summary report
SUMMARY_FILE="$OUTPUT_DIR/summary.txt"
{
    echo "Valgrind Test Summary  $(date '+%Y-%m-%d %H:%M:%S')"
    echo "========================================"
    echo -e "$SUMMARY"
    echo "----------------------------------------"
    echo "PASS: $PASS  FAIL: $FAIL  SKIP: $SKIP  TOTAL: ${#TESTS[@]}"
} > "$SUMMARY_FILE"

echo ""
echo "========================================"
echo "PASS: $PASS  FAIL: $FAIL  SKIP: $SKIP  TOTAL: ${#TESTS[@]}"
echo "Summary: $SUMMARY_FILE"

[[ $FAIL -eq 0 ]]
