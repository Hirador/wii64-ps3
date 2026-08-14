# Build environment for wii64-ps3.
#
# Base image supplies ps3toolchain (GCC 7.2.0 / powerpc64-ps3-elf) and PSL1GHT
# under /usr/local/ps3dev, but not NVIDIA's Cg runtime. PSL1GHT's `cgcomp`
# dlopen()s libCg.so at runtime rather than linking it, so without the runtime
# every shader target dies with "Unable to load Cg, aborting." and the build
# stops before compiling a single C file.
#
# NVIDIA discontinued the Cg Toolkit in 2012; the April 2012 release is the
# last one and is still served from developer.download.nvidia.com.
FROM scrapes/ps3toolchain-minimal:latest

ARG CG_URL=https://developer.download.nvidia.com/cg/Cg_3.1/Cg-3.1_April2012_x86_64.tgz

RUN wget -q -O /tmp/cg.tgz "$CG_URL" \
 && tar -xzf /tmp/cg.tgz -C /tmp ./usr/lib64/libCg.so ./usr/lib64/libCgGL.so \
 && mv /tmp/usr/lib64/libCg.so /tmp/usr/lib64/libCgGL.so /usr/lib/x86_64-linux-gnu/ \
 && rm -rf /tmp/cg.tgz /tmp/usr \
 && ldconfig \
 && cgcomp -v /dev/null /dev/null 2>&1 | grep -qi "unable to load cg" \
    && { echo "FATAL: libCg still not loadable by cgcomp"; exit 1; } || true

# The base image ships no portlibs at all, but main/adler32.c (a verbatim copy
# of zlib's own) includes <zlib.h>, and upstream Wii64 uses zlib for zipped ROM
# loading. Cross-compile it into $PS3DEV/portlibs/ppu, which Makefile.ps3
# already has on its include and library paths.
#
# Pinned to the GitHub release asset rather than zlib.net, which removes
# tarballs for superseded versions (zlib.net/zlib-1.3.1.tar.gz is already 404).
ARG ZLIB_VERSION=1.3.1
ARG ZLIB_URL=https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz

RUN wget -q -O /tmp/zlib.tar.gz "$ZLIB_URL" \
 && tar -xzf /tmp/zlib.tar.gz -C /tmp \
 && cd "/tmp/zlib-${ZLIB_VERSION}" \
 && CHOST=powerpc64-ps3-elf \
    CC=powerpc64-ps3-elf-gcc \
    AR=powerpc64-ps3-elf-ar \
    RANLIB=powerpc64-ps3-elf-ranlib \
    CFLAGS="-O2 -mcpu=cell" \
    ./configure --static --prefix=/usr/local/ps3dev/portlibs/ppu \
 && make -j"$(nproc)" \
 && make install \
 && rm -rf /tmp/zlib.tar.gz "/tmp/zlib-${ZLIB_VERSION}" \
 && test -f /usr/local/ps3dev/portlibs/ppu/include/zlib.h \
 && test -f /usr/local/ps3dev/portlibs/ppu/lib/libz.a

WORKDIR /src/wii64
