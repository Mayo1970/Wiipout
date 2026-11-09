FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    git cmake ninja-build g++ pkg-config libasound2-dev libavcodec-dev libavformat-dev \
    libavutil-dev libbluetooth-dev libcurl4-openssl-dev libevdev-dev libfmt-dev \
    libmbedtls-dev libminiupnpc-dev libpugixml-dev libsfml-dev libswscale-dev \
    libusb-1.0-0-dev libxxhash-dev libzstd-dev zlib1g-dev ca-certificates && \
    rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --recurse-submodules --shallow-submodules --branch 2506 https://github.com/dolphin-emu/dolphin.git /dolphin
RUN apt-get update && apt-get install -y --no-install-recommends libudev-dev && rm -rf /var/lib/apt/lists/*
RUN cmake -S /dolphin -B /dolphin/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/dolphin -DENABLE_NOGUI=ON -DENABLE_QT=OFF \
    -DENABLE_X11=OFF -DENABLE_WAYLAND=OFF -DENABLE_SDL=OFF -DENABLE_TESTS=OFF \
    -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF -DENABLE_LLVM=OFF -DENABLE_AUTOUPDATE=OFF && \
    cmake --build /dolphin/build && cmake --install /dolphin/build

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    mtools dosfstools libasound2t64 libavcodec60 libavformat60 libavutil58 \
    libbluetooth3 libcurl4t64 libevdev2 libfmt9 libmbedtls14 libminiupnpc17 \
    libpugixml1v5 libsfml-system2.6 libswscale7 libusb-1.0-0 libxxhash0 libzstd1 \
    zlib1g libudev1 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/dolphin /opt/dolphin
ENV PATH="/opt/dolphin/bin:${PATH}"
