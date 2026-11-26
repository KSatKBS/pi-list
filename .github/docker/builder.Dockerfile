# Build environment for EBU LIST (C++ analyzer + Node.js server + React GUI).
# Used by .github/workflows/docker-image.yml to produce the release/ directory.

ARG BASE_IMAGE=ubuntu:20.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
        build-essential ninja-build git wget curl ca-certificates file zip bzip2 xz-utils rsync \
        python3 python3-pip python3-venv pkg-config \
        libpcap-dev libssl-dev uuid-dev && \
    rm -rf /var/lib/apt/lists/*

# Conan 1.x (the project uses the legacy cmake generator)
RUN python3 -m venv /opt/venv && \
    /opt/venv/bin/python -m pip install --no-cache-dir --upgrade pip setuptools wheel && \
    /opt/venv/bin/python -m pip install --no-cache-dir "conan<2" && \
    ln -s /opt/venv/bin/conan /usr/local/bin/conan

ENV CONAN_SYSREQUIRES_SUDO=0
ENV CONAN_SYSREQUIRES_MODE=enabled

ARG CMAKE_VERSION=3.19.8
RUN wget -q -O /tmp/cmake.sh https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-Linux-x86_64.sh && \
    sh /tmp/cmake.sh --skip-license --prefix=/usr/local && rm -f /tmp/cmake.sh

# Node.js 20 (official binary) + yarn (classic)
ARG NODE_VERSION=20.18.1
RUN wget -q -O /tmp/node.tar.xz https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz && \
    tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 && rm -f /tmp/node.tar.xz && \
    npm install -g yarn@1

RUN git config --global --add safe.directory '*'
