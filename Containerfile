# llvm4msvc 0.9.3, pinned to the public amd64 manifest used by the Linux-host
# cross-build (https://github.com/highcanfly-club/llvm4msvc). Override
# MMVP_BASE_IMAGE when using an equivalent local image.
ARG MMVP_BASE_IMAGE=docker.io/highcanfly/llvm4msvc@sha256:4fc95bf6099825c2f5546e1a216b83a70b657ec6491eef33f398a9dea85fdd20
FROM ${MMVP_BASE_IMAGE}

USER root
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        git \
        nasm \
        ninja-build \
        pkg-config \
        python3 \
        tar \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf Version.Lib /usr/share/msvc/sdk/lib/um/x86_64/Version.lib \
    && ln -sf /usr/bin/lld-link /usr/local/bin/link.exe \
    && ln -sf /usr/bin/llvm-rc /usr/local/bin/rc.exe

ARG VCPKG_COMMIT=cc288af760054fa489574bd8e22d05aa8fa01e5c
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout "${VCPKG_COMMIT}" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

COPY cmake/tool-wrappers/lib.exe /usr/local/bin/lib.exe
RUN chmod 755 /usr/local/bin/lib.exe

ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache \
    CL="" \
    LINK=""

WORKDIR /work

RUN test -x /usr/bin/clang-cl \
    && test -x /usr/bin/lld-link \
    && test -x /usr/bin/llvm-rc \
    && test -x /opt/vcpkg/vcpkg \
    && test -x /usr/local/bin/lib.exe
