# Dockerfile for HOGD CI Environment
# Dual-use: Local testing + Namespace.so custom base image
#
# For local use:
#   docker build -t hogd-ci .
#
# For Namespace.so custom base image:
#   Replace BASE_IMAGE default with ${NAMESPACE_BASE_IMAGE_REF}

ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

# Avoid prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install prerequisites for adding LLVM repository
# Note: Use sudo for Namespace.so compatibility (their base image runs as non-root)
RUN sudo apt-get update && sudo apt-get install -y --no-install-recommends \
    ca-certificates \
    wget \
    gnupg \
    && sudo rm -rf /var/lib/apt/lists/*

# Add LLVM repository for clang-21
RUN wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" | sudo tee /etc/apt/sources.list.d/llvm.list

# Install all build dependencies
RUN sudo apt-get update && sudo apt-get install -y --no-install-recommends \
    # Build tools
    build-essential \
    cmake \
    make \
    ccache \
    flex \
    bison \
    git \
    openssh-client \
    # Clang/LLVM 21 toolchain
    clang-21 \
    clang-tidy-21 \
    clang-tools-21 \
    libc++-21-dev \
    libc++abi-21-dev \
    lld \
    llvm-21 \
    # GCC 14 toolchain
    gcc-14 \
    g++-14 \
    # Python tools
    python3 \
    python3-flake8 \
    # Libraries
    libjemalloc-dev \
    libssl-dev \
    lz4 \
    liblz4-dev \
    libsnappy-dev \
    zlib1g-dev \
    libbz2-dev \
    libzstd-dev \
    && sudo rm -rf /var/lib/apt/lists/*

# Set up update-alternatives for clang-21 as default
RUN sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 200 \
    && sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 200 \
    && sudo update-alternatives --set clang /usr/bin/clang-21 \
    && sudo update-alternatives --set clang++ /usr/bin/clang++-21 \
    && sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-21 100 \
    && sudo update-alternatives --install /usr/bin/run-clang-tidy run-clang-tidy /usr/bin/run-clang-tidy-21 100

# Set up LLVM tools alternatives
RUN for tool in llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-objdump llvm-strip llvm-config; do \
        if [ -f /usr/bin/${tool}-21 ]; then \
            sudo update-alternatives --install /usr/bin/${tool} ${tool} /usr/bin/${tool}-21 100; \
        fi; \
    done

# Install Rust (stable toolchain)
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH

# Install Rust toolchains:
# - 1.90.0 (required by CMakeLists.txt for hogd/rust)
# - stable (general use)
# - nightly-2025-09-24 (required by built_contracts for wasm32 + build-std)
RUN sudo mkdir -p /usr/local/rustup /usr/local/cargo \
    && sudo chmod 777 /usr/local/rustup /usr/local/cargo \
    && wget -qO- https://sh.rustup.rs | sh -s -- -y --default-toolchain 1.90.0 --profile minimal \
    && rustup toolchain install stable \
    && rustup toolchain install nightly-2025-09-24 \
    && rustup target add wasm32-unknown-unknown --toolchain nightly-2025-09-24 \
    && rustup component add rust-src --toolchain nightly-2025-09-24 \
    && rustup component add clippy --toolchain 1.90.0 \
    && rustup component add clippy --toolchain stable \
    && cargo install cbindgen \
    && sudo chmod -R a+rx /usr/local/rustup /usr/local/cargo

# Create non-root user for local development (skipped on Namespace.so which has its own user)
ARG UID=1000
ARG GID=1000
RUN sudo groupadd -g ${GID} builder 2>/dev/null || true \
    && sudo useradd -m -u ${UID} -g ${GID} -s /bin/bash builder 2>/dev/null || true

# Add GitHub to known_hosts for SSH access (works for any user via $HOME)
RUN mkdir -p ~/.ssh \
    && ssh-keyscan github.com >> ~/.ssh/known_hosts 2>/dev/null \
    && chmod 700 ~/.ssh && chmod 600 ~/.ssh/known_hosts

# Set working directory
WORKDIR /workspace

# Default command for interactive use
CMD ["/bin/bash"]
