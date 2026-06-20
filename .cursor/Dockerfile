# Cursor Cloud Agent environment for HOGD.
# Based on .devcontainer/Dockerfile; do not COPY the project (Cursor checks out commits).

ARG BASE_IMAGE=ubuntu:26.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

# Cloud agents require git and sudo in custom images.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    wget \
    gnupg \
    software-properties-common \
    sudo \
    git \
    openssh-client \
    && rm -rf /var/lib/apt/lists/*

# Add LLVM repository for clang-21
RUN wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/resolute/ llvm-toolchain-resolute-21 main" | tee /etc/apt/sources.list.d/llvm.list

# Install build dependencies (gcc-15 is the default in Ubuntu 26.04).
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    make \
    ccache \
    flex \
    bison \
    clang-21 \
    clangd-21 \
    clang-format-21 \
    clang-tidy-21 \
    clang-tools-21 \
    libc++-21-dev \
    libc++abi-21-dev \
    lld-21 \
    llvm-21 \
    gcc-15 \
    g++-15 \
    python3 \
    python3-yaml \
    python3-flake8 \
    shfmt \
    wabt \
    binaryen \
    gdb \
    libjemalloc-dev \
    libssl-dev \
    lz4 \
    liblz4-dev \
    libsnappy-dev \
    zlib1g-dev \
    libbz2-dev \
    libzstd-dev \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 200 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 200 \
    && update-alternatives --set clang /usr/bin/clang-21 \
    && update-alternatives --set clang++ /usr/bin/clang++-21 \
    && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-21 100 \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-21 100 \
    && update-alternatives --install /usr/bin/run-clang-tidy run-clang-tidy /usr/bin/run-clang-tidy-21 100 \
    && update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-21 100

RUN for tool in llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-objdump llvm-strip llvm-config; do \
        if [ -f /usr/bin/${tool}-21 ]; then \
            update-alternatives --install /usr/bin/${tool} ${tool} /usr/bin/${tool}-21 100; \
        fi; \
    done \
    && update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-21 100 \
    && update-alternatives --install /usr/bin/lld lld /usr/bin/lld-21 100

ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH \
    CARGO_NET_GIT_FETCH_WITH_CLI=true

# Keep toolchains in sync with:
#   hogd/rust/rust-toolchain.toml (1.93.0)
#   built_contracts/rust-toolchain.toml (nightly-2026-02-12)
RUN mkdir -p /usr/local/rustup /usr/local/cargo \
    && chmod 777 /usr/local/rustup /usr/local/cargo \
    && wget -qO- https://sh.rustup.rs | sh -s -- -y --default-toolchain 1.93.0 --profile minimal \
    && rustup component add clippy rustfmt --toolchain 1.93.0 \
    && rustup toolchain install 1.96.0 --profile minimal \
    && rustup component add clippy rustfmt --toolchain 1.96.0 \
    && rustup target add wasm32-unknown-unknown riscv32im-unknown-none-elf --toolchain 1.96.0 \
    && rustup toolchain install nightly-2026-02-12 --profile minimal \
    && rustup target add wasm32-unknown-unknown --toolchain nightly-2026-02-12 \
    && rustup component add rust-src clippy rustfmt --toolchain nightly-2026-02-12 \
    && cargo install --locked --version 0.29.2 cbindgen \
    && chmod -R a+rwx /usr/local/rustup /usr/local/cargo

# Cloud agents run as the ubuntu user.
RUN id -u ubuntu &>/dev/null || useradd -m -s /bin/bash ubuntu \
    && usermod -aG sudo ubuntu \
    && echo "ubuntu ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/ubuntu \
    && mkdir -p /home/ubuntu/.cache/ccache /home/ubuntu/.ssh \
    && ssh-keyscan github.com >> /home/ubuntu/.ssh/known_hosts 2>/dev/null \
    && chmod 700 /home/ubuntu/.ssh \
    && chown -R ubuntu:ubuntu /home/ubuntu

WORKDIR /workspace
CMD ["/bin/bash"]
