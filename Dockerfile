# west ビルド環境コンテナ (kas-container の west 版)
# 使い方は west-container.sh 経由 (直接 docker run も可)。
# Zephyr SDK は使わず apt の gcc-arm-none-eabi (gnuarmemb variant)。
FROM ubuntu:24.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        cmake ninja-build device-tree-compiler git \
        gcc-arm-none-eabi libnewlib-arm-none-eabi \
        python3 python3-pip python3-venv \
        ca-certificates file \
    && rm -rf /var/lib/apt/lists/*

# Zephyr の Python 依存 (west.yml で pin している v4.3.0 の要求に合わせる)
ADD https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/v4.3.0/scripts/requirements-base.txt /tmp/req.txt
RUN pip install --break-system-packages --no-cache-dir west pyelftools -r /tmp/req.txt

ENV ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
    GNUARMEMB_TOOLCHAIN_PATH=/usr
