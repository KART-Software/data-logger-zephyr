#!/bin/sh
# try.sh — ビルド済み ELF を実機で「試す」(仮ロード)。scp して remoteproc で
# M コア (8MM の M4 / 8MP の M7) に載せるだけなので、リブートで元のファームに戻る
# (SPL が起動する確定済みのファーム。8MP は p3 の mcore-a/b.img、8MM は boot の m4-fw.img)。
# 恒久配備は scripts/install.sh。
#
# 使い方:
#   ./scripts/try.sh                          # build-cangw-container の zephyr.elf を m4-fw.elf として
#   ./scripts/try.sh build-hello              # 別ビルドディレクトリを指定
#   ./scripts/try.sh build-hello hello.elf    # 転送先ファイル名も指定
#   BOARD=root@<ip> ./scripts/try.sh          # 接続先の変更 (既定は tailnet の XPI)
#   BOARD=root@192.168.0.7 ./scripts/try.sh build-cangw-8mp   # DEBIX (8MP) で試す
#
# 挙動:
#   - ELF は /lib/firmware に置く (rootfs を一時 rw remount。firmware_class の
#     path パラメータは使わない — kmm-yocto pitfalls #24)
#   - M4 が稼働中 (running/attached) なら先に stop する (稼働中は firmware を
#     変更できない)。BL31 先住起動のボードでも、この stop→LOAD で差し替え可能。
#     ただし eMMC に置いたファームは不変なのでリブートで元に戻る
#   - 起動後 M コア経由の CAN (rpmsgcan0) の出現を待ち、DOWN なら /etc/default/can0 の
#     ビットレートで上げる。8MP の can0 は Linux 側の FlexCAN2 で別物なので触らない
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

# M コア経由の CAN (rpmsg-can の rpmsgcan0) の出現待ち (最大 5s)。can-gw 以外の
# ファームでは出ないので警告だけ
IF=rpmsgcan0
BITRATE=1000000
[ -f /etc/default/can0 ] && . /etc/default/can0
i=0
while [ ! -e "/sys/class/net/$IF" ] && [ "$i" -lt 50 ]; do
    i=$((i + 1)); sleep 0.1
done
if [ -e "/sys/class/net/$IF" ]; then
    if ! ip link show "$IF" | grep -q "state UP"; then
        ip link set "$IF" type can bitrate "$BITRATE" 2>/dev/null || true
        ip link set "$IF" up 2>/dev/null || true
    fi
    echo "$IF: $(ip -br link show "$IF")"
else
    echo "$IF: 出現せず (can-gw 以外のファームなら正常)"
fi
EOF
echo "==> done. M コアのログ: 8MM は /dev/kart-m4-uart (115200)、8MP は機体の /sys/kernel/debug/remoteproc/remoteproc0/trace0"
