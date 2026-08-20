#!/bin/sh
# install.sh — ビルド済み M4 ファームを実機に「恒久配備」する (Yocto 不要)。
# zephyr.bin をヘッダ付きコンテナ m4-fw.img に包み、boot パーティションへ
# scp する。次回リブートから SPL が読み BL31 が M4 を起動する。
#
# 一時的に試すだけなら scripts/try.sh (remoteproc、リブートで戻る)。
# 必ず try.sh で動作確認してから install すること (RDC 違反級のバグ入り
# ファームを恒久化すると SoC リセットループになり得る)。
#
# 使い方:
#   ./scripts/install.sh                     # build-cangw-container の zephyr.bin
#   ./scripts/install.sh build-hello         # 別ビルドディレクトリ
#   ./scripts/install.sh build --reboot      # 配備後にリブートまで行う
#   BOARD=root@<ip> ./scripts/install.sh     # 接続先変更 (既定は tailnet の XPI)
#
# ヘッダ形式 (SPL パッチ 0011 / kart-falcon-itb.bb と一致必須):
#   0x00 magic "K4FW" / 0x04 payload長 / 0x08 payload CRC32 / 0x0C version (各 LE32)
set -eu

BOARD="${BOARD:-root@100.87.109.114}"
BUILD_DIR="build-cangw-container"
DO_REBOOT=0
for a in "$@"; do
    case "$a" in
        --reboot) DO_REBOOT=1 ;;
        *) BUILD_DIR="$a" ;;
    esac
done
BIN="$BUILD_DIR/zephyr/zephyr.bin"
[ -f "$BIN" ] || { echo "ERROR: $BIN が無い (先に west build)" >&2; exit 1; }

IMG=$(mktemp /tmp/m4-fw.XXXXXX.img)
trap 'rm -f "$IMG"' EXIT
python3 - "$BIN" "$IMG" << 'PYEOF'
import struct, sys, time, zlib
payload = open(sys.argv[1], 'rb').read()
hdr = b'K4FW' + struct.pack('<III', len(payload),
                            zlib.crc32(payload) & 0xffffffff, int(time.time()))
open(sys.argv[2], 'wb').write(hdr + payload)
print(f"m4-fw.img: {len(payload)} bytes payload, crc32 ok")
PYEOF

echo "==> $BIN -> $BOARD:/boot/m4-fw.img"
scp -q "$IMG" "$BOARD:/tmp/m4-fw.img"
ssh "$BOARD" sh -s << 'EOF'
set -eu
mount -o remount,rw /boot
cp /tmp/m4-fw.img /boot/m4-fw.img
sync
mount -o remount,ro /boot
echo "installed: $(wc -c < /boot/m4-fw.img) bytes at /boot/m4-fw.img"
EOF

if [ "$DO_REBOOT" = 1 ]; then
    echo "==> reboot"
    ssh "$BOARD" reboot || true
else
    echo "==> 次回リブートから有効 (今すぐ試すなら scripts/try.sh)"
fi
