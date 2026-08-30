FROM --platform=linux/arm64 ubuntu:24.04
LABEL org.graft64.stage="G1-placeholder"
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates clang cmake git ninja-build python3 pkg-config && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
ENTRYPOINT ["/bin/bash"]
