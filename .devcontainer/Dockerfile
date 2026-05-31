# Dockerfile for HOGD CI Environment
# Dual-use: Local testing + Namespace.so custom base image
#
# For local use:
#   docker build -t hogd-ci .
#
# For Namespace.so custom base image:
#   Replace BASE_IMAGE default with ${NAMESPACE_BASE_IMAGE_REF}
#
# Note: Uses conditional sudo - skipped when running as root (local Docker),
# used when running as non-root (Namespace.so base image)

ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

# Avoid prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install prerequisites for adding LLVM repository
# Note: Uses conditional sudo - empty when root (local), "sudo" when non-root (Namespace.so)
RUN if [ "$(id -u)" = "0" ]; then SUDO=""; else SUDO="sudo"; fi && \
    $SUDO apt-get update && $SUDO apt-get install -y --no-install-recommends \
    ca-certificates \
    wget \
    gnupg \
    software-properties-common \
    && $SUDO rm -rf /var/lib/apt/lists/*

# Add LLVM repository for clang-21
RUN if [ "$(id -u)" = "0" ]; then SUDO=""; else SUDO="sudo"; fi && \
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | $SUDO tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" | $SUDO tee /etc/apt/sources.list.d/llvm.list

# Add the toolchain PPA for gcc-15 (noble's default archive only ships gcc-14;
# gcc-15 is needed for __attribute__((musttail)) used by the rv32 threaded
# interpreter).
RUN if [ "$(id -u)" = "0" ]; then SUDO=""; else SUDO="sudo"; fi && \
    $SUDO add-apt-repository -y ppa:ubuntu-toolchain-r/test

# Install all build dependencies
RUN if [ "$(id -u)" = "0" ]; then SUDO=""; else SUDO="sudo"; fi && \
    $SUDO apt-get update && $SUDO apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    make \
    ccache \
    flex \
    bison \
    git \
    openssh-client \
    clang-21 \
    clangd-21 \
    clang-format-21 \
    clang-tidy-21 \
    clang-tools-21 \
    libc++-21-dev \
    libc++abi-21-dev \
    lld-21 \
    llvm-21 \
    gcc-14 \
    g++-14 \
    gcc-15 \
    g++-15 \
    python3 \
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
    && $SUDO rm -rf /var/lib/apt/lists/*

# Set up update-alternatives for clang-21 as default
RUN if [ "$(id -u)" = "0" ]; then SUDO=""; else SUDO="sudo"; fi && \
    $SUDO update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 200 \
    && $SUDO update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 200 \
    && $SUDO update-alternatives --set clang /usr/bin/clang-21 \
    && $SUDO update-alternatives --set clang++ /usr/bin/clang++-21 \
    && $SUDO update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-21 100 \
    && $SUDO update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-21 100 \
    && $SUDO update-alternatives --install /usr/bin/run-clang-tidy run-clang-tidy /usr/bin/run-clang-tidy-21 100 \
    && $SUDO update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-21 100

# Set up LLVM tools alternatives
RUN if [ "$(id -u)" = "0" ]; then SUDO=""; else SUDO="sudo"; fi && \
    for tool in llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-objdump llvm-strip llvm-config; do \
        if [ -f /usr/bin/${tool}-21 ]; then \
            $SUDO update-alternatives --install /usr/bin/${tool} ${tool} /usr/bin/${tool}-21 100; \
        fi; \
    done && \
    $SUDO update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-21 100 && \
    $SUDO update-alternatives --install /usr/bin/lld lld /usr/bin/lld-21 100

# Install Rust (stable toolchain)
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH

# Install Rust toolchains:
# - 1.90.0 (required by CMakeLists.txt for hogd/rust)
# - stable (general use)
# - nightly-2025-09-24 (required by built_contracts for wasm32 + build-std)
RUN if [ "$(id -u)" = "0" ]; then SUDO=""; else SUDO="sudo"; fi && \
    $SUDO mkdir -p /usr/local/rustup /usr/local/cargo \
    && $SUDO chmod 777 /usr/local/rustup /usr/local/cargo \
    && wget -qO- https://sh.rustup.rs | sh -s -- -y --default-toolchain 1.90.0 --profile minimal \
    && rustup toolchain install stable \
    && rustup toolchain install nightly-2025-09-24 \
    && rustup target add wasm32-unknown-unknown --toolchain nightly-2025-09-24 \
    && rustup component add rust-src --toolchain nightly-2025-09-24 \
    && rustup component add clippy --toolchain nightly-2025-09-24 \
    && rustup component add rustfmt --toolchain nightly-2025-09-24 \
    && rustup component add clippy --toolchain 1.90.0 \
    && rustup component add rustfmt --toolchain 1.90.0 \
    && rustup component add clippy --toolchain stable \
    && rustup component add rustfmt --toolchain stable \
    && cargo install cbindgen \
    && $SUDO chmod -R a+rwx /usr/local/rustup /usr/local/cargo

# Create non-root user for local development
RUN useradd -m -s /bin/bash builder \
    && mkdir -p /home/builder/.cache/ccache /home/builder/.ssh \
    && ssh-keyscan github.com >> /home/builder/.ssh/known_hosts 2>/dev/null \
    && chmod 700 /home/builder/.ssh \
    && chmod 600 /home/builder/.ssh/known_hosts || true \
    && chown -R builder:builder /home/builder

# Add GitHub to known_hosts for root
RUN mkdir -p ~/.ssh \
    && ssh-keyscan github.com >> ~/.ssh/known_hosts 2>/dev/null \
    && chmod 700 ~/.ssh && chmod 600 ~/.ssh/known_hosts

# Set working directory
WORKDIR /workspace

# Default command for interactive use
CMD ["/bin/bash"]
