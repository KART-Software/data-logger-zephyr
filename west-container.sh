#!/bin/bash
# west-container.sh — west を Docker 内で実行する薄いラッパー (kas-container 風)。
#
#   ./west-container.sh update
#   ./west-container.sh build -b imx8mm_evk/mimx8mm6/m4 apps/hello-world
#   ./west-container.sh --shell          # コンテナ内シェル
#
# 仕組み:
# - リポジトリルート (このスクリプトのある場所) を「ホストと同一の絶対パス」で
#   マウントする → CMake キャッシュ内の絶対パスがホスト/コンテナで共用でき、
#   どちらでビルドしても混在可能
# - ホストの uid/gid で実行 → 生成物の所有者がホストユーザーのまま
# - イメージは Dockerfile のハッシュでタグ付けし、変更時のみ再ビルド
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TAG="kart-west:$(sha256sum "$HERE/Dockerfile" | head -c 12)"

if ! docker image inspect "$TAG" >/dev/null 2>&1; then
    echo "[west-container] building image $TAG ..." >&2
    docker build -t "$TAG" -f "$HERE/Dockerfile" "$HERE"
fi

DOCKER_ARGS=(
    --rm
    -v "$HERE:$HERE"
    -w "$HERE"
    -u "$(id -u):$(id -g)"
    -e HOME=/tmp
    -e ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
    -e GNUARMEMB_TOOLCHAIN_PATH=/usr
)
# 対話端末があるときだけ -it (CI でも使えるように)
if [ -t 0 ]; then
    DOCKER_ARGS+=(-it)
fi

if [ "${1:-}" = "--shell" ]; then
    exec docker run "${DOCKER_ARGS[@]}" "$TAG" bash
fi

exec docker run "${DOCKER_ARGS[@]}" "$TAG" west "$@"
