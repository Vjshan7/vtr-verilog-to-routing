FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive

# ------------------------------------------------------------------------------
#  1. Base setup
# ------------------------------------------------------------------------------
ENV WORKSPACE=/workspace
RUN mkdir -p ${WORKSPACE}
WORKDIR ${WORKSPACE}
COPY . ${WORKSPACE}

# Allow pip to install system-wide (Ubuntu 23+ restriction)
ENV PIP_BREAK_SYSTEM_PACKAGES=1

# ------------------------------------------------------------------------------
#  2. Install dependencies
# ------------------------------------------------------------------------------
RUN apt-get update -qq && \
    apt-get -y install --no-install-recommends \
        dos2unix \
        wget \
        git \
        ninja-build \
        python3-pip \
        python3-venv \
        build-essential \
        cmake \
        pkg-config \
        bison \
        flex \
        libxml2-utils \
        libtbb-dev \
        libeigen3-dev \
        libx11-dev \
        libxrender-dev \
        libxrandr-dev \
        libxi-dev \
        libxft-dev \
        libxext-dev \
        libglu1-mesa-dev \
        libgl1-mesa-dev \
        libgtk-3-dev \
        openssl \
        libssl-dev \
        time && \
    apt-get autoclean && apt-get clean && apt-get -y autoremove && \
    rm -rf /var/lib/apt/lists/*

# ------------------------------------------------------------------------------
#  3. Install Python requirements (if present)
# ------------------------------------------------------------------------------
RUN if [ -f requirements.txt ]; then pip install -r requirements.txt; fi

# ------------------------------------------------------------------------------
#  4. Initialize submodules (to get arch + benchmarks)
# ------------------------------------------------------------------------------
RUN git submodule update --init --recursive

# ------------------------------------------------------------------------------
#  5. Build VTR cleanly using CMake
# ------------------------------------------------------------------------------
RUN rm -rf build && mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && \
    make install

# ------------------------------------------------------------------------------
#  6. Environment setup
# ------------------------------------------------------------------------------
ENV PATH="${WORKSPACE}/build/vpr:${WORKSPACE}/build/vtr:${WORKSPACE}/vtr_flow/scripts:${PATH}"
WORKDIR ${WORKSPACE}
SHELL ["/bin/bash", "-c"]

# ------------------------------------------------------------------------------
#  7. Default interactive mode
# ------------------------------------------------------------------------------
ENTRYPOINT ["/bin/bash"]
CMD ["-i"]
