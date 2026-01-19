FROM devkitpro/devkita64:latest AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    autoconf \
    meson \
    bison \
    flex \
    libtool \
    pkg-config \
    git \
    gimp \
    python3 \
    python3-pip \
    python3-mako

RUN dkp-pacman -Syu --noconfirm \
    switch-bzip2 \
    switch-dav1d \
    switch-freetype \
    switch-glm \
    switch-harfbuzz \
    switch-libarchive \
    switch-libass \
    switch-libfribidi \
    switch-libjpeg-turbo \
    switch-libpng \
    switch-libwebp \
    switch-libssh2 \
    switch-mbedtls \
    switch-ntfs-3g \
    switch-lwext4 \
    switch-pkg-config \
    dkp-meson-scripts \
    dkp-toolchain-vars

RUN git clone --recurse-submodules https://github.com/auggeythecat/SwitchWave.git

WORKDIR /SwitchWave

RUN git clone --recurse-submodules https://github.com/DarkMatterCore/libusbhsfs.git && \
    cd libusbhsfs && \
    make BUILD_TYPE=GPL install

RUN git clone                      https://github.com/sahlberg/libsmb2.git && \
    cd libsmb2 && \
    make -f Makefile.platform switch_install

RUN ls /opt/devkitpro

RUN wget https://github.com/sahlberg/libnfs/archive/refs/tags/libnfs-5.0.2.tar.gz && \
    tar -xzf libnfs-5.0.2.tar.gz && \
    cd libnfs-libnfs-5.0.2 && \
    patch -Np1 -i ../misc/libnfs/switch.patch && \
    . /opt/devkitpro/switchvars.sh && \
    ./bootstrap && \
    ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf --without-libkrb5 \
    --disable-shared --enable-static --disable-werror --disable-utils --disable-examples && \
    make && \
    make install && \
    install -Dm644 COPYING "/opt/devkitpro/portlibs/switch/licenses/switch-libnfs/LICENSE"

RUN make configure-ffmpeg && \
    make build-ffmpeg -j$(nproc)

RUN make configure-uam && \
    make build-uam

RUN make configure-mpv && \
    make build-mpv

RUN make dist -j$(nproc)

FROM scratch

COPY --from=build /SwitchWave/build /
