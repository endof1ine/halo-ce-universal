# The Switch build environment (port/switch/README.md): devkitA64 and libnx
# for the host, and a current clang for the arm64_32 guest. Build it with
# tools/switch_docker.sh.

FROM devkitpro/devkita64:latest

ARG LLVM_VERSION=21

RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		ca-certificates curl gnupg lsb-release git ninja-build python3 python3-pytest rsync \
		imagemagick xz-utils \
	&& curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
	&& echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-${LLVM_VERSION} main" \
		> /etc/apt/sources.list.d/llvm.list \
	&& apt-get update \
	&& apt-get install -y --no-install-recommends \
		clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} libclang-rt-${LLVM_VERSION}-dev \
	&& rm -rf /var/lib/apt/lists/*

ENV PATH="/usr/lib/llvm-${LLVM_VERSION}/bin:${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:${PATH}"

# FFmpeg for the game's movies, Bink only and under the LGPL
# (port/switch/host/host_bink.c)
COPY build_ffmpeg.sh /tmp/build_ffmpeg.sh
RUN sh /tmp/build_ffmpeg.sh /opt/halo-ffmpeg && rm /tmp/build_ffmpeg.sh

WORKDIR /src
