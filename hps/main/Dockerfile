# SPDX-License-Identifier: GPL-3.0-or-later
FROM debian:bookworm-slim
ARG TC_URL=https://developer.arm.com/-/media/Files/downloads/gnu-a/10.2-2020.11/binrel/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf.tar.xz
ARG TC_SHA256=102825ae56c9e00142d06f35d2bdd3299edb6060e84a275a25b095e66fd3fc2a
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl xz-utils make file \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL -o /tmp/tc.tar.xz "$TC_URL" \
    && echo "$TC_SHA256  /tmp/tc.tar.xz" | sha256sum -c - \
    && tar -C /opt -xJf /tmp/tc.tar.xz \
    && rm /tmp/tc.tar.xz
ENV PATH=/opt/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf/bin:$PATH
