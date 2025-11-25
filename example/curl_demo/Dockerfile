FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libcurl4-openssl-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire project
COPY . .

# Build
WORKDIR /app/build
RUN cmake -DBUILD_EXAMPLES=ON -DBUILD_CURL_EXAMPLE=ON -DBUILD_TESTING=OFF .. && \
    make -j$(nproc)

# The executable will be in example/curl_demo/

