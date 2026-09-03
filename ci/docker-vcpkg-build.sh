#!/usr/bin/env bash
# Configure, build and test the checked-out agent source with the `vcpkg`
# preset. Installed as /usr/local/bin/pinpoint-vcpkg-build in the image built
# from ci/Dockerfile.vcpkg, where the dependency closure is already in the vcpkg
# binary cache.
#
#   pinpoint-vcpkg-build [source-dir]
#
# PINPOINT_BUILD_JOBS  parallelism for the compile step (default: nproc)
# PINPOINT_CTEST_ARGS  extra ctest arguments, e.g. "-E agent_integration_test"
set -euo pipefail

source_dir="${1:-$PWD}"
jobs="${PINPOINT_BUILD_JOBS:-$(nproc)}"
cd "${source_dir}"

test -f CMakePresets.json || {
    echo "pinpoint-vcpkg-build: no CMakePresets.json in ${source_dir}" >&2
    echo "Mount or check out the agent source there first." >&2
    exit 2
}

# Timed per phase so a CI log shows immediately whether a slow run means a
# cache miss on the dependencies (configure) or the agent's own code (build).
step() {
    local label="$1"
    shift
    local start
    start=$(date +%s)
    echo "==> ${label}"
    "$@"
    echo "==> ${label} finished in $(($(date +%s) - start))s"
}

total_start=$(date +%s)
step "configure (vcpkg)" cmake --preset vcpkg
step "build (${jobs} jobs)" cmake --build --preset vcpkg --parallel "${jobs}"
step "test" ctest --preset vcpkg ${PINPOINT_CTEST_ARGS:-}
echo "==> total $(($(date +%s) - total_start))s"
