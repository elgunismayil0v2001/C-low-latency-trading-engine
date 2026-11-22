FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG TARGETARCH

# Package versions are pinned to the Ubuntu 24.04 toolchain used by CI.
RUN if [ "$TARGETARCH" = "amd64" ]; then \
      gcc_package_version=13.3.0-6ubuntu2~24.04.1; \
    else \
      gcc_package_version=13.2.0-23ubuntu4; \
    fi \
    && apt-get update \
    && apt-get install --yes --no-install-recommends \
      cmake=3.28.3-1build7 \
      "g++-13=$gcc_package_version" \
      clang-18=1:18.1.3-1ubuntu1 \
      clang-format-18=1:18.1.3-1ubuntu1 \
      clang-tidy-18=1:18.1.3-1ubuntu1 \
      libclang-rt-18-dev=1:18.1.3-1ubuntu1 \
      ccache \
      git \
      libgtest-dev \
      make \
      ninja-build \
      zlib1g-dev \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 180 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 180 \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 180 \
    && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-18 180 \
    && rm -rf /var/lib/apt/lists/*

ENV CCACHE_DIR=/root/.cache/ccache
ENV CXX=clang++
WORKDIR /workspace
