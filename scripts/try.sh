#!/bin/sh
# try.sh — ビルド済み ELF を実機で「試す」(仮ロード)。scp して remoteproc で
# M4 に載せるだけなので、リブートで元 (falcon.itb 内のファーム) に戻る。
# 本番への恒久配備は kmm-yocto の falcon loadable (kas/imx8mm-m4.yml)。
#
# 使い方:
#   ./scripts/try.sh                          # build-cangw-container の zephyr.elf を m4-fw.elf として
#   ./scripts/try.sh build-hello              # 別ビルドディレクトリを指定
#   ./scripts/try.sh build-hello hello.elf    # 転送先ファイル名も指定
#   BOARD=root@<ip> ./scripts/try.sh          # 接続先の変更 (既定は tailnet の XPI)
#
# 挙動:
#   - ELF は /lib/firmware に置く (rootfs を一時 rw remount。firmware_class の
#     path パラメータは使わない — kmm-yocto pitfalls #24)
#   - M4 が稼働中 (running/attached) なら先に stop する (稼働中は firmware を
#     変更できない)。BL31 先住起動のボードでも、この stop→LOAD で差し替え可能。
#     ただし flash 内 (falcon.itb) のファームは不変なのでリブートで元に戻る
#   - 起動後 can0 の出現を待ち、DOWN なら bringup する (can0-up が居れば不要)
set -eu

BOARD="${BOARD:-root@100.87.109.114}"
BUILD_DIR="${1:-build-cangw-container}"
NAME="${2:-m4-fw.elf}"
ELF="$BUILD_DIR/zephyr/zephyr.elf"

[ -f "$ELF" ] || { echo "ERROR: $ELF が無い (先に west build)" >&2; exit 1; }

echo "==> $ELF ($(wc -c < "$ELF") bytes) -> $BOARD:/lib/firmware/$NAME"
scp -q "$ELF" "$BOARD:/tmp/$NAME"

# リモート側は busybox sh 前提 (POSIX 構文のみ)
ssh "$BOARD" sh -s "$NAME" <<'EOF'
set -eu
NAME="$1"
R=/sys/class/remoteproc/remoteproc0

mount -o remount,rw /
cp "/tmp/$NAME" "/lib/firmware/$NAME"
sync
mount -o remount,ro /

state=$(cat "$R/state")
if [ "$state" != "offline" ]; then
    echo -n stop > "$R/state"
    sleep 1
fi
echo -n "$NAME" > "$R/firmware"
echo -n start > "$R/state"
sleep 2
echo "remoteproc: $(cat "$R/state") ($NAME)"

# can0 出現待ち (最大 5s)。can-gw 以外のファームでは出ないので警告だけ
i=0
while [ ! -e /sys/class/net/can0 ] && [ "$i" -lt 50 ]; do
    i=$((i + 1)); sleep 0.1
done
if [ -e /sys/class/net/can0 ]; then
    if ! ip link show can0 | grep -q "state UP"; then
        ip link set can0 type can bitrate 1000000 2>/dev/null || true
        ip link set can0 up 2>/dev/null || true
    fi
    echo "can0: $(ip -br link show can0)"
else
    echo "can0: 出現せず (can-gw 以外のファームなら正常)"
fi
EOF
echo "==> done. M4 コンソール: /dev/kart-m4-uart (115200)"
