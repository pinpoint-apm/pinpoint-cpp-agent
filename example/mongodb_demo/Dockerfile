FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
# We need to install mongo-cxx-driver.
# Since it's not always available in apt with the latest versions, we might need to build it or use available packages.
# Ubuntu 22.04 has libmongocxx-dev but it might be old. Let's try standard apt first.
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libcurl4-openssl-dev \
    libmongocxx-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire project
COPY . .

# Build
WORKDIR /app/build
# We enable BUILD_MONGODB_EXAMPLE
RUN cmake -DBUILD_EXAMPLES=ON -DBUILD_MONGODB_EXAMPLE=ON -DBUILD_TESTING=OFF .. && \
    make -j$(nproc)

# The executable will be in example/mongodb_demo/

