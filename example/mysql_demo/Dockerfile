FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libssl-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Install MySQL Connector/C++ 8.0
WORKDIR /tmp
RUN wget https://dev.mysql.com/get/Downloads/Connector-C++/mysql-connector-c++-8.0.33-linux-glibc2.28-x86-64bit.tar.gz && \
    tar -xzf mysql-connector-c++-8.0.33-linux-glibc2.28-x86-64bit.tar.gz && \
    cd mysql-connector-c++-8.0.33-linux-glibc2.28-x86-64bit && \
    cp -r include/* /usr/local/include/ && \
    cp -r lib64/* /usr/local/lib/ && \
    ldconfig && \
    cd .. && \
    rm -rf mysql-connector-c++-8.0.33-linux-glibc2.28-x86-64bit*

WORKDIR /app

# Copy the entire project
COPY . .

# Build
WORKDIR /app/build
RUN cmake -DBUILD_EXAMPLES=ON -DBUILD_MYSQL_EXAMPLE=ON -DBUILD_TESTING=OFF .. && \
    make -j$(nproc)

# The executable will be in example/mysql_demo/
EXPOSE 8089

CMD ["/app/build/example/mysql_demo/db_demo"]

