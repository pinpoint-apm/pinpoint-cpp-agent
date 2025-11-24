FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
# Note: we install dependencies required for pinpoint-cpp agent as well
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libcurl4-openssl-dev \
    librdkafka-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire project to build the library and examples
COPY . .

# Build
WORKDIR /app/build
# We configure with BUILD_EXAMPLES=ON and BUILD_KAFKA_EXAMPLE=ON
# The example/CMakeLists.txt will add the kafka_demo subdirectory
RUN cmake -DBUILD_EXAMPLES=ON -DBUILD_KAFKA_EXAMPLE=ON -DBUILD_TESTING=OFF .. && \
    make -j$(nproc)

# The executables will be in example/kafka_demo/


