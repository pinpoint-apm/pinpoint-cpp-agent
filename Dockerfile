# syntax=docker/dockerfile:1
# Unified Linux build and test environment. Select a preset at container run:
#   docker build -t pinpoint-cpp-agent-test .
#   docker run --rm pinpoint-cpp-agent-test default
#   docker run --rm pinpoint-cpp-agent-test vcpkg
#   docker run --rm pinpoint-cpp-agent-test bazel
#
# Supported CMake presets: default, debug, vcpkg, asan, tsan,
# ubsan, coverage, profiling.
# Supported Bazel presets: bazel, bazel-asan, bazel-tsan, bazel-ubsan,
# bazel-profiling.

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG BAZEL_VERSION=7.4.1
ARG VCPKG_BASELINE=b781af668027bbf77f2f827f47b5c6cd8d825c08

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        curl \
        git \
        libclang-rt-dev \
        llvm \
        ninja-build \
        perl \
        pkg-config \
        tar \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/* \
    && case "$(dpkg --print-architecture)" in \
        amd64) BAZEL_ARCH=x86_64 ;; \
        arm64) BAZEL_ARCH=arm64 ;; \
        *) echo "Unsupported Docker architecture: $(dpkg --print-architecture)" >&2; exit 1 ;; \
       esac \
    && curl --fail --location --silent --show-error \
        --output /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-${BAZEL_ARCH}" \
    && chmod +x /usr/local/bin/bazel \
    && git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout "${VCPKG_BASELINE}" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
WORKDIR /workspace

COPY . .

# The image copies the checked-out IDL submodule as ordinary source files.
# The entrypoint configures, builds, and tests the selected mode: any CMake
# preset name from CMakePresets.json, `bazel`, or `bazel-<config>` for the
# .bazelrc configs. `coverage` generates its report as part of its build
# target; profiling needs a direct build of the configured directory so every
# CTest executable exists.
ENTRYPOINT ["bash", "-ec", "preset=\"${1:-default}\"; case \"$preset\" in coverage) cmake --preset coverage; cmake --build --preset coverage --parallel \"$(nproc)\" ;; profiling) cmake --preset profiling; cmake --build build/profiling --parallel \"$(nproc)\"; ctest --test-dir build/profiling --output-on-failure ;; bazel) bazel build //...; bazel test //test/... --test_output=errors ;; bazel-*) bazel build --config=\"${preset#bazel-}\" //...; bazel test --config=\"${preset#bazel-}\" //test/... --test_output=errors ;; *) cmake --preset \"$preset\"; cmake --build --preset \"$preset\" --parallel \"$(nproc)\"; ctest --preset \"$preset\" ;; esac", "pinpoint-cpp-agent-test"]
CMD ["default"]
