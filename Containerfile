FROM localhost/status-effect-in-game-time-builder:latest

USER root
COPY cmake/tool-wrappers/lib.exe /usr/local/bin/lib.exe
RUN apt-get update \
    && apt-get install -y --no-install-recommends nasm \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/lld-link /usr/local/bin/link.exe \
    && ln -s /usr/bin/llvm-rc /usr/local/bin/rc.exe \
    && chmod 755 /usr/local/bin/lib.exe
