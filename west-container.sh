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

# ---- patches/<module>/*.patch を各 west モジュールに冪等適用 ----
# west には bitbake の SRC_URI パッチ相当が無いので、関所であるこのラッパーが
# 肩代わりする。適用済み (reverse-check が通る) なら skip、未適用なら apply、
# どちらも通らなければ「モジュール更新でパッチが古い」ので警告して止める。
# 現在の中身: zephyr の spi_mcux_ecspi 修正 2 本 (GPIO CS + reg!=0 のチャネル
# 選択不整合で ISR が来ないバグ / タイムアウト時に SDK handle が busy 固着して
# バス全体が死ぬバグ)。どちらも実機 (MCP2515+ADS8688 @ ECSPI2) で顕在化・修正。
apply_patches() {
    local dir mod p
    for dir in "$HERE"/patches/*/; do
        [ -d "$dir" ] || continue
        mod=$(basename "$dir")
        [ -d "$HERE/$mod" ] || continue      # west update 前はまだ無い
        for p in "$dir"*.patch; do
            [ -f "$p" ] || continue
            if git -C "$HERE/$mod" apply --reverse --check "$p" 2>/dev/null; then
                continue                     # 適用済み
            elif git -C "$HERE/$mod" apply --check "$p" 2>/dev/null; then
                echo "[west-container] applying $(basename "$p") -> $mod" >&2
                git -C "$HERE/$mod" apply "$p"
            else
                echo "[west-container] ERROR: $(basename "$p") が $mod に適用できない (モジュール更新とのズレ — パッチのリベースが必要)" >&2
                exit 1
            fi
        done
    done
}
apply_patches
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

rc=0
docker run "${DOCKER_ARGS[@]}" "$TAG" west "$@" || rc=$?
# west update はモジュールをリセットするので、直後にパッチを当て直す
case "${1:-}" in
    update|init) apply_patches ;;
esac
exit $rc
