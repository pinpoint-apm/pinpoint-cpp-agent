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

# A container CPU limit is a CFS quota, not a smaller cpuset, so nproc still
# reports every core on the host and -j$(nproc) oversubscribes a capped runner
# by a wide margin: 20 compilers sharing four cores' worth of quota thrash
# memory and stall. Prefer the quota when the cgroup declares one.
detect_jobs() {
    local quota period
    if [ -r /sys/fs/cgroup/cpu.max ]; then                    # cgroup v2
        read -r quota period < /sys/fs/cgroup/cpu.max
    elif [ -r /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]; then     # cgroup v1
        quota=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us)
        period=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us)
    fi
    case "${quota:-}" in
        ''|max|-1|0) nproc; return ;;
    esac
    # Round down, but never below 1: --cpus=0.5 must not yield -j0.
    echo $(( quota / period > 0 ? quota / period : 1 ))
}

source_dir="${1:-$PWD}"
jobs="${PINPOINT_BUILD_JOBS:-$(detect_jobs)}"
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
