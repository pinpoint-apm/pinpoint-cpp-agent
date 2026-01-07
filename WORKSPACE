workspace(name = "pinpoint-cpp")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# rules_proto
http_archive(
    name = "rules_proto",
    sha256 = "14a225870ab4e91869652cfd69ef2028277fc1dc4910d65d353b62d6e0ae21f4",
    strip_prefix = "rules_proto-7.1.0",
    url = "https://github.com/bazelbuild/rules_proto/releases/download/7.1.0/rules_proto-7.1.0.tar.gz",
)

load("@rules_proto//proto:repositories.bzl", "rules_proto_dependencies")

rules_proto_dependencies()

load("@rules_proto//proto:toolchains.bzl", "rules_proto_toolchains")

rules_proto_toolchains()

http_archive(
    name = "rules_cc",
    sha256 = "712d77868b3152dd618c4d64faaddefcc5965f90f5de6e6dd1d5ddcd0be82d42",
    strip_prefix = "rules_cc-0.1.1",
    urls = ["https://github.com/bazelbuild/rules_cc/releases/download/0.1.1/rules_cc-0.1.1.tar.gz"],
)

http_archive(
    name = "com_google_protobuf",
    sha256 = "63150aba23f7a90fd7d87bdf514e459dd5fe7023fdde01b56ac53335df64d4bd",
    strip_prefix = "protobuf-29.2",
    urls = ["https://github.com/protocolbuffers/protobuf/releases/download/v29.2/protobuf-29.2.tar.gz"],
)

load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")

protobuf_deps()

http_archive(
    name = "com_github_grpc_grpc",
    sha256 = "5b5b9c6507ec166ec0e9e82f58280c36544540ecdd818eaab7b8601596b74c9e",
    strip_prefix = "grpc-1.63.2",
    urls = ["https://github.com/grpc/grpc/archive/v1.63.2.tar.gz"],
)

load("@com_github_grpc_grpc//bazel:grpc_deps.bzl", "grpc_deps")

grpc_deps()

http_archive(
    name = "com_github_jbeder_yaml_cpp",
    sha256 = "fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16",
    strip_prefix = "yaml-cpp-0.8.0",
    urls = ["https://github.com/jbeder/yaml-cpp/archive/0.8.0.tar.gz"],
)

http_archive(
    name = "com_github_gabime_spdlog",
    sha256 = "9962648c9b4f1a7bbc76fd8d9172555bad1871fdb14ff4f842ef87949682caa5",
    strip_prefix = "spdlog-1.15.0",
    urls = ["https://github.com/gabime/spdlog/archive/v1.15.0.tar.gz"],
)

http_archive(
    name = "com_google_absl",
    sha256 = "f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3",
    strip_prefix = "abseil-cpp-20240722.0",
    urls = ["https://github.com/abseil/abseil-cpp/archive/20240722.0.tar.gz"],
)
