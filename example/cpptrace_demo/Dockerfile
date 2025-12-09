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
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire project
COPY . .

# Build cpptrace from source
WORKDIR /app/build-cpptrace
RUN git clone https://github.com/jeremy-rifkin/cpptrace.git && \
    cd cpptrace && \
    git checkout v1.0.4 && \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local && \
    cmake --build build -j$(nproc) && \
    cmake --install build && \
    cd .. && rm -rf cpptrace

# Build the project
WORKDIR /app/build
RUN cmake -DBUILD_EXAMPLES=ON -DBUILD_TESTING=OFF -Dcpptrace_DIR=/usr/local/lib/cmake/cpptrace .. && \
    make -j$(nproc)

# The executable will be in example/cpptrace_demo/
EXPOSE 8088

CMD ["/app/build/example/cpptrace_demo/cpptrace_demo"]

