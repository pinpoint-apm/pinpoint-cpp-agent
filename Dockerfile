# syntax=docker/dockerfile:1
# Unified Linux build and test environment. Select a preset at container run:
#   docker build -t pinpoint-cpp-agent-test .
#   docker run --rm pinpoint-cpp-agent-test default
#   docker run --rm pinpoint-cpp-agent-test vcpkg
#   docker run --rm pinpoint-cpp-agent-test bazel
#
# Supported CMake presets: default, debug, debug-cached, vcpkg, asan, tsan,
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
        ccache \
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
# The entrypoint configures, builds, and tests the selected mode. `coverage`
# generates its report as part of its build target; profiling needs a direct
# build of the configured directory so every CTest executable exists.
ENTRYPOINT ["bash", "-ec", "preset=\"${1:-default}\"; case \"$preset\" in default|debug|debug-cached|vcpkg|asan|tsan|ubsan) cmake --preset \"$preset\"; cmake --build --preset \"$preset\" --parallel \"$(nproc)\"; ctest --preset \"$preset\" ;; coverage) cmake --preset coverage; cmake --build --preset coverage --parallel \"$(nproc)\" ;; profiling) cmake --preset profiling; cmake --build build/profiling --parallel \"$(nproc)\"; ctest --test-dir build/profiling --output-on-failure ;; bazel) bazel build //...; bazel test //test/... --test_output=errors ;; bazel-asan) bazel build --config=asan //...; bazel test --config=asan //test/... --test_output=errors ;; bazel-tsan) bazel build --config=tsan //...; bazel test --config=tsan //test/... --test_output=errors ;; bazel-ubsan) bazel build --config=ubsan //...; bazel test --config=ubsan //test/... --test_output=errors ;; bazel-profiling) bazel build --config=profiling //...; bazel test --config=profiling //test/... --test_output=errors ;; help|--help|-h) echo 'Presets: default, debug, debug-cached, vcpkg, asan, tsan, ubsan, coverage, profiling, bazel, bazel-asan, bazel-tsan, bazel-ubsan, bazel-profiling' ;; *) echo \"Unknown preset: $preset\" >&2; exit 2 ;; esac", "pinpoint-cpp-agent-test"]
CMD ["default"]
