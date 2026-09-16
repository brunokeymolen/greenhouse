FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    bison \
    ca-certificates \
    ccache \
    cmake \
    flex \
    gcc \
    git \
    gperf \
    libc6-dev \
    libffi-dev \
    libncurses-dev \
    libssl-dev \
    make \
    ninja-build \
    python3 \
    python-is-python3 \
    python3-pip \
    python3-serial \
    python3-setuptools \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Keep the toolchain outside /root so the container can also run as a normal
# host user (see docker/run.sh) without needing a writable /root.
ENV IDF_TOOLS_PATH=/opt/espressif

WORKDIR /opt

RUN git clone --recursive --branch release/v3.4 \
    https://github.com/espressif/ESP8266_RTOS_SDK.git

WORKDIR /opt/ESP8266_RTOS_SDK

RUN ./install.sh

# install.sh seeds the SDK's virtualenv with the newest setuptools, but
# setuptools 81 removed pkg_resources, which both export.sh and the SDK's
# check_python_dependencies.py still import. Pin it back.
RUN "$IDF_TOOLS_PATH"/python_env/*/bin/python -m pip install "setuptools<81"

# The SDK builds its Kconfig frontends inside IDF_PATH on first use, which a
# non-root container user cannot write to. Build them once, here, as root.
RUN make -C /opt/ESP8266_RTOS_SDK/tools/kconfig conf-idf mconf-idf

# export.sh runs `git describe` on the SDK checkout to pick its Python env.
# That repo is root-owned, so a non-root container user trips git's dubious
# ownership check. Trust it for every user in this image.
RUN git config --system --add safe.directory /opt/ESP8266_RTOS_SDK

# Readable and executable by any uid, so --user works.
RUN chmod -R a+rX "$IDF_TOOLS_PATH" /opt/ESP8266_RTOS_SDK

ENV IDF_PATH=/opt/ESP8266_RTOS_SDK
ENV PATH="/opt/ESP8266_RTOS_SDK/tools:${PATH}"

WORKDIR /project

CMD ["/bin/bash"]
