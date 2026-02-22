FROM devkitpro/devkita64:20260215

SHELL ["/bin/bash", "-c"]

# Build tools + GIMP (for BCn texture conversion)
RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake libtool \
    bison flex \
    python3-pip python3-mako ninja-build \
    wget ca-certificates \
    gimp \
    && pip3 install --break-system-packages meson \
    && rm -rf /var/lib/apt/lists/*

ENV DEVKITPRO=/opt/devkitpro
ENV PORTLIBS_PREFIX=${DEVKITPRO}/portlibs/switch

# Build libusbhsfs (GPL)
RUN git clone --depth 1 https://github.com/DarkMatterCore/libusbhsfs.git /tmp/libusbhsfs \
    && cd /tmp/libusbhsfs \
    && source ${DEVKITPRO}/switchvars.sh \
    && make BUILD_TYPE=gpl install \
    && rm -rf /tmp/libusbhsfs

# Build libsmb2
RUN git clone --depth 1 https://github.com/sahlberg/libsmb2.git /tmp/libsmb2 \
    && cd /tmp/libsmb2 \
    && make -f Makefile.platform switch_install \
    && rm -rf /tmp/libsmb2

# Build libnfs v5.0.2
COPY misc/libnfs/switch.patch /tmp/libnfs-switch.patch
RUN wget -qO- https://github.com/sahlberg/libnfs/archive/refs/tags/libnfs-5.0.2.tar.gz | tar xz -C /tmp \
    && cd /tmp/libnfs-libnfs-5.0.2 \
    && patch -Np1 -i /tmp/libnfs-switch.patch \
    && source ${DEVKITPRO}/switchvars.sh \
    && ./bootstrap \
    && ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf \
        --disable-shared --enable-static --disable-werror --disable-utils --disable-examples \
    && make && make install \
    && rm -rf /tmp/libnfs-libnfs-5.0.2 /tmp/libnfs-switch.patch

WORKDIR /mnt
