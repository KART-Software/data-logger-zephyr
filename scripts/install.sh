#!/bin/sh
# install.sh — ビルド済みの M コアのファームを実機に「恒久配備」する (Yocto 不要)。
# zephyr.bin をヘッダ付きのコンテナに包んで機体へ送り、機体の A/B に合わせて置く。
#
#   A/B 対応の SPL の機体 (DEBIX。kmm-yocto の `ab spl update` で SPL を更新済み):
#     機体の `ab mcore update` で、M コア専用パーティション p3 の反対の面に書く。
#     次の起動で 1 回だけ試し、rpmsg まで動けば自動で確定、駄目なら確定済みの面に戻る。
#     試すのは 1 回きりなので、壊れたファームでも SoC リセットのループにはならない
#   旧 SPL の機体 (XPI):
#     boot パーティションの m4-fw.img (8MP なら m7-fw.img) を置き換える。次の起動から
#     そのまま使われ、自動では戻らない。必ず先に scripts/try.sh で動作を確かめること
#     (RDC 違反級のバグ入りファームを恒久化すると SoC リセットループになり得る)
#
# 一時的に試すだけなら scripts/try.sh (remoteproc、リブートで戻る)。
#
# 使い方:
#   ./scripts/install.sh                            # build-cangw-container の zephyr.bin
#   ./scripts/install.sh build-cangw-8mp            # 別ビルドディレクトリ
#   ./scripts/install.sh build-cangw-8mp --reboot   # 配備後に再起動まで行う
#   BOARD=root@192.168.0.7 ./scripts/install.sh build-cangw-8mp   # 接続先 (既定は tailnet の XPI)
#
# ヘッダの magic はビルドの SoC (zephyr/.config) で決める: 8MP の M7 = M7FW、8MM の M4 = M4FW。
# 機体が期待する magic (機体の ab の定数、無ければホスト名) と合わなければ止める。
# ヘッダ形式 (SPL のパッチ / kmm-yocto の falcon-itb.bb と ab と一致必須):
#   0x00 magic / 0x04 payload長 / 0x08 payload CRC32 / 0x0C version (各 LE32)
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
CONF="$BUILD_DIR/zephyr/.config"
[ -f "$BIN" ] || { echo "ERROR: $BIN が無い (先に west build)" >&2; exit 1; }

if grep -q '^CONFIG_SOC_MIMX8ML8_M7=y' "$CONF" 2>/dev/null; then
    MAGIC=M7FW; LEGACY_NAME=m7-fw.img
elif grep -q '^CONFIG_SOC_MIMX8MM6_M4=y' "$CONF" 2>/dev/null; then
    MAGIC=M4FW; LEGACY_NAME=m4-fw.img
else
    echo "ERROR: $CONF から SoC が分からない (8MP の M7 か 8MM の M4 のビルドだけ扱う)" >&2
    exit 1
fi
hex() { printf %s "$1" | od -An -tx1 | tr -d ' \n'; }

# 機体の A/B の種類 (ab / legacy)、機体が期待する magic、ホスト名を調べる。
# A/B 対応の SPL は、動いている SPL のコピーに ab の印がある (ab spl status の SPL_<面>_AB=1)
INFO=$(ssh -n -o BatchMode=yes "$BOARD" '
kind=legacy; m=
if command -v ab >/dev/null 2>&1; then
    m=$(sed -n "s/^MCORE_MAGIC=\([0-9a-f]*\).*/\1/p" "$(command -v ab)")
    s=$(ab spl status 2>/dev/null || true)
    c=$(echo "$s" | sed -n "s/^SPL_RUNNING_COPY=//p")
    if [ -n "$c" ] && echo "$s" | grep -q "^SPL_${c}_AB=1"; then kind=ab; fi
fi
echo "$kind ${m:--} $(hostname)"')
set -- $INFO
KIND=$1; WANT=$2; HOST=$3
if [ "$WANT" = - ]; then
    case "$HOST" in
        imx8mp*) WANT=$(hex M7FW) ;;
        imx8mm*) WANT=$(hex M4FW) ;;
    esac
fi
if [ "$WANT" != "$(hex "$MAGIC")" ]; then
    echo "ERROR: $BUILD_DIR は $MAGIC 向けのビルドだが、機体 $HOST の M コアとは合わない" >&2
    exit 1
fi

IMG=$(mktemp /tmp/mcore-fw.XXXXXX.img)
trap 'rm -f "$IMG"' EXIT
python3 - "$BIN" "$IMG" "$MAGIC" << 'PYEOF'
import struct, sys, time, zlib
payload = open(sys.argv[1], 'rb').read()
hdr = sys.argv[3].encode() + struct.pack('<III', len(payload),
                                         zlib.crc32(payload) & 0xffffffff, int(time.time()))
open(sys.argv[2], 'wb').write(hdr + payload)
print(f"{sys.argv[3]}: {len(payload)} bytes payload, crc32 ok")
PYEOF

if [ "$KIND" = ab ]; then
    echo "==> $BIN -> $BOARD (ab mcore update、$HOST)"
    scp -q "$IMG" "$BOARD:/tmp/mcore-fw.img"
    NR="--no-reboot"
    [ "$DO_REBOOT" = 1 ] && NR=""
    r=0
    ssh -n "$BOARD" "ab mcore update /tmp/mcore-fw.img $NR; r=\$?; rm -f /tmp/mcore-fw.img; exit \$r" || r=$?
    # 再起動で接続が切れたのは成功 (ssh の 255)
    if [ "$r" -ne 0 ] && ! { [ "$DO_REBOOT" = 1 ] && [ "$r" -eq 255 ]; }; then
        exit "$r"
    fi
    if [ "$DO_REBOOT" = 1 ]; then
        echo "==> 再起動しました。結果は機体で ab mcore status (MCORE_SLOT が新しい面なら確定済み)"
    else
        echo "==> 次の起動で 1 回試し、動けば自動で確定します (今すぐなら機体を再起動)"
    fi
    exit 0
fi

echo "==> $BIN -> $BOARD:/boot/$LEGACY_NAME (旧 SPL の機体、$HOST)"
scp -q "$IMG" "$BOARD:/tmp/$LEGACY_NAME"
ssh "$BOARD" sh -s "$LEGACY_NAME" << 'EOF'
set -eu
NAME="$1"
mount -o remount,rw /boot
cp "/tmp/$NAME" "/boot/$NAME"
rm -f "/tmp/$NAME"
sync
mount -o remount,ro /boot
echo "installed: $(wc -c < "/boot/$NAME") bytes at /boot/$NAME"
EOF

if [ "$DO_REBOOT" = 1 ]; then
    echo "==> reboot"
    ssh "$BOARD" reboot || true
else
    echo "==> 次回リブートから有効 (今すぐ試すなら scripts/try.sh)"
fi
