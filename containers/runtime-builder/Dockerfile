FROM --platform=linux/arm64 ubuntu:24.04
LABEL org.graft64.stage="G1"
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    autoconf bison build-essential ca-certificates clang cmake curl flex gettext git \
    libasound2-dev libfontconfig1-dev libfreetype6-dev libfuse3-dev libpng-dev \
    libssl-dev ninja-build pkg-config python3 tar xz-utils && rm -rf /var/lib/apt/lists/*
ARG LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260826/llvm-mingw-20260826-ucrt-ubuntu-22.04-aarch64.tar.xz"
ARG LLVM_MINGW_SHA256="4eb475cccf5e5e37ea3b693a52227e70a86ae70abafceb9ecd83887e67699c9d"
RUN set -eux; \
    curl --fail --location --silent --show-error "$LLVM_MINGW_URL" --output /tmp/llvm-mingw.tar.xz; \
    echo "$LLVM_MINGW_SHA256  /tmp/llvm-mingw.tar.xz" | sha256sum --check --status; \
    mkdir -p /opt/llvm-mingw; \
    tar --extract --file /tmp/llvm-mingw.tar.xz --directory /opt/llvm-mingw --strip-components=1; \
    rm -f /tmp/llvm-mingw.tar.xz
ENV PATH="/opt/llvm-mingw/bin:${PATH}"
WORKDIR /workspace
ENTRYPOINT ["/bin/bash"]
