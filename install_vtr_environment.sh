#!/bin/bash
# ============================================================
# VTR / VPR Environment Setup Script (for DSE_FPGA repo)
# Compatible with Ubuntu 22.04 / 24.04
# Uses submodule at: vtr-verilog-to-routing/
# Mirrors the Dockerfile setup
# ============================================================

set -e  # Exit on first error
set -o pipefail

echo "==========================================="
echo " Setting up VTR/VPR Environment (Linux)"
echo " Repo: DSE_FPGA / vtr-verilog-to-routing"
echo "==========================================="

# --------------------------------------------
# 1. Update & install core system tools
# --------------------------------------------
echo "[1/6] Updating package index..."
sudo apt-get update -qq

echo "[2/6] Installing core build tools..."
sudo apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    wget \
    curl \
    pkg-config \
    dos2unix \
    python3 \
    python3-pip \
    python3-venv \
    python3-dev \
    time

# --------------------------------------------
# 2. Install dependencies required by VTR
# --------------------------------------------
echo "[3/6] Installing C++ and graphics dependencies..."
sudo apt-get install -y --no-install-recommends \
    bison \
    flex \
    libxml2-utils \
    libtbb-dev \
    libeigen3-dev \
    libgtk-3-dev \
    libx11-dev \
    libxrender-dev \
    libxrandr-dev \
    libxi-dev \
    libxft-dev \
    libxext-dev \
    libglu1-mesa-dev \
    libgl1-mesa-dev \
    libreadline-dev \
    libffi-dev \
    libboost-system-dev \
    libboost-python-dev \
    libboost-filesystem-dev \
    default-jre \
    zlib1g-dev \
    openssl \
    libssl-dev

# --------------------------------------------
# 3. Optional clang-format (if available)
# --------------------------------------------
if apt-cache search '^clang-format-18$' | grep -q 'clang-format-18'; then
    echo "[4/6] Installing clang-format-18..."
    sudo apt-get install -y clang-format-18
else
    echo "[4/6] clang-format-18 not found, skipping..."
fi

# --------------------------------------------
# 4. Ensure submodule exists
# --------------------------------------------
if [ ! -d "vtr-verilog-to-routing" ]; then
    echo "[5/6] ERROR: vtr-verilog-to-routing submodule not found!"
    echo "Please initialize it first using:"
    echo "  git submodule update --init --recursive"
    exit 1
else
    echo "[5/6] vtr-verilog-to-routing found, proceeding with build..."
fi

# Navigate into the submodule
cd vtr-verilog-to-routing

# --------------------------------------------
# 5. Build VTR with CMake
# --------------------------------------------
echo "[6/6] Building VTR..."
rm -rf build
mkdir build && cd build

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install

# --------------------------------------------
# 6. Success message
# --------------------------------------------
echo "==========================================="
echo " VTR/VPR successfully built and installed!"
echo "==========================================="
echo "You can test it using:"
echo ""
echo "  ./build/vpr/vpr vtr_flow/arch/timing/k6_N10_mem32K_40nm.xml \\"
echo "      vtr_flow/benchmarks/blif/alu4.blif --disp on"
echo ""
echo "If you’re on a GUI-enabled system, VPR’s graphics window will appear."
echo ""
