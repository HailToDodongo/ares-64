# syntax=docker/dockerfile:1
# libdragon-ares-test: N64 homebrew CI image combining the libdragon toolchain
# (+ libdragon library/tools built from a local checkout) with the ares-test
# headless emulator test runner (JS-scripted, no GPU/display needed).
#
# The libdragon source tree is passed as an extra build context:
#
# Build:  podman build --build-context libdragon=../libdragon -t libdragon-ares-test .
# Use:    podman run --rm -v "$PWD":/app -w /app libdragon-ares-test make
#         podman run --rm -v "$PWD":/app -w /app libdragon-ares-test ares-test test.js game.z64

ARG BASE_IMAGE=ghcr.io/dragonminded/libdragon:latest

# Stage 1 - Build libdragon + host tools from the libdragon build context.
# Same sequence as libdragon's build.sh, minus the examples
FROM ${BASE_IMAGE} AS libdragon-builder

COPY --from=libdragon . /libdragon
WORKDIR /libdragon

RUN make -j"$(nproc)" install-mk \
    && make -j"$(nproc)" clobber \
    && make -j"$(nproc)" libdragon tools \
    && make -j"$(nproc)" install tools-install

# Stage 2 - Build ares-test from this repository
# Built FROM the same base image (ubuntu:22.04) so the binary links against the glibc/libstdc++ of the final stage. 
FROM ${BASE_IMAGE} AS ares-builder

RUN --mount=target=/var/lib/apt/lists,type=cache,sharing=locked \
    --mount=target=/var/cache/apt,type=cache,sharing=locked \
    apt update \
    && apt install -y --no-install-recommends g++-12 gcc-12 ninja-build python3-pip \
    && pip3 install --no-cache-dir "cmake>=3.28,<4"

COPY . /ares-64
WORKDIR /ares-64

RUN CC=gcc-12 CXX=g++-12 cmake --preset linux-headless -DENABLE_CCACHE=OFF \
    && cmake --build build_headless --target ares-test -j"$(nproc)" \
    && mkdir -p /opt/ares-test \
    && cp build_headless/test-runner/ares-test /opt/ares-test/ \
    && cp -r "build_headless/test-runner/Firmware" /opt/ares-test/Firmware

# Stage 3 - Final image without the source trees and build artifacts
FROM ${BASE_IMAGE}

COPY --from=libdragon-builder ${N64_INST} ${N64_INST}
COPY --from=ares-builder /opt/ares-test /opt/ares-test
RUN ln -s /opt/ares-test/ares-test /usr/local/bin/ares-test
