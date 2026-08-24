/*
 * can-gw — MCP2515 (ECSPI2) ⇄ rpmsg "kart-can" の双方向 CAN ゲートウェイ。
 *
 * Linux 側は kmm-yocto の kart-rpmsg-can モジュールが NS 告知で bind して
 * CAN netdev rpcan0 を登録する。以後 Linux アプリは SocketCAN 無修正。
 *
 *   CAN rx (ISRコールバック) → msgq → gw スレッド → rpmsg → rpcan0
 *   rpcan0 送信 → rpmsg → ept コールバック → can_send → CAN バス
 *
 * ワイヤ形式 (kart-rpmsg-can.c と共通):
 *   - データフレーム 16B: { uint32 id; uint8 dlc; uint8 pad[3]; uint8 data[8] } (LE)
 *     id は Linux canid_t の慣例 (bit31=EFF, bit30=RTR)。
 *   - 制御メッセージ 8B: { uint8 magic=0xC7; uint8 cmd; uint8 flags; uint8 rsvd;
 *     uint32 arg }。長さで区別。Linux 側 candev の netlink 設定を反映する:
 *     bitrate/mode を受けて can_set_bitrate/can_set_mode、START/STOP で
 *     can_start/can_stop。M4→Linux は状態変化 (bus-off 等) を EVT_STATE で返す。
 *   - それ以外の長さ (Linux probe の "hi" 等) は返信先アドレス学習にだけ使う。
 *
 * CAN の起動は Linux の `ip link up` (= START) 契機。M4 起動時は can_start せず、
 * bitrate/mode を停止中に受けてから start する (SocketCAN candev の流儀)。
 *
 * rpmsg 部は apps/rpmsg-echo (openamp_rsc_table サンプル) と同じ構成。
 * imx_rproc との噛み合わせ 2 点 (ライブリソーステーブル / MU チャネル 1
 * 固定) もそのまま — 詳細コメントは各所に残してある。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/spi.h>
#include "kart_can.h"	/* kart-can 生成物 (can.yaml 単一ソース) */
#include <zephyr/drivers/ipm.h>
#include <string.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>
#include <addr_translation.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(can_gw);

/* 前提 (RDC): M4 が使うペリフェラル (ECSPI2/GPIO3/GPIO5) は、M4 起動前に
 * Linux (domain0) 側から RDC PDAP を M4 専用 (0x0C) に設定しておくこと。
 * 共有 (0xFF) のままだと、MU/IPM を含むビルドではペリフェラル READ が
 * バスエラー → SoC ハードリセットになる (実測)。M4 自身からの RDC 書込は
 * 黙って無視されるため必ず Linux 側で行う:
 *   devmem 0x303D0408 32 0x0C   # GPIO3
 *   devmem 0x303D0410 32 0x0C   # GPIO5
 *   devmem 0x303D058C 32 0x0C   # ECSPI2
 */

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "can-gw requires definition of shared memory for rpmsg"
#endif

/* ---- ECSPI2 クロックを M4 自身で enable ----
 * Linux 側 DT は ecspi2 を disabled にした (M4 譲渡)。i.MX8MM の CCGR は
 * 4bit/domain のドメイン別要求フィールドで、domain0 (Linux) の devmem では
 * domain1 (M4) のフィールドに書けない (実測: 0x33 を書くと 0x03 になる)。
 * domain1 の要求が立っていないと M4 のレジスタアクセスで bus stall →
 * SoC ハードリセットになる (実測)。SDK の CLOCK_EnableClock と同じく
 * SET レジスタに全ドメイン分 (0x3333) を書く。M4 (domain1) からの write は
 * 自ドメインのフィールドに有効。ECSPI ドライバ init (banner 前) より先に
 * 走るよう PRE_KERNEL_1 priority 0 で行う。
 * TARGET_ROOT は Linux 側 (mcore_booted=1 + clk_ignore_unused) が enable を
 * 維持する取り決め (kmm-yocto imx8mm-xpi-kart.dts のコメント参照)。 */
#define CCM_CCGR_ECSPI2_SET  0x30384084u	/* CCGR (0x4080) の SET */
#define CCM_CCGR_GPIO3_SET   0x303840D4u	/* CCGR (0x40D0) の SET */
#define CCM_CCGR_GPIO5_SET   0x303840F4u	/* CCGR (0x40F0) の SET */
#define CCM_CCGR_UART4_SET   0x303844C4u	/* CCGR76 (0x44C0) の SET — kmm-yocto pitfalls #25 実測 */

static int kart_m4_clocks_enable(void)
{
	/* clk-test (kmm-yocto m4/clk-test) での実測: M4 write は domain1
	 * フィールドのみに効き (0x3333 → 読み値 0x30)、これで ECSPI2 read が
	 * 2700 万回生還。can-gw が使う GPIO3 (INT)/GPIO5 (CS)、コンソールの
	 * UART4 も同様に立てる */
	*(volatile uint32_t *)CCM_CCGR_ECSPI2_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_GPIO3_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_GPIO5_SET = 0x3333;
	*(volatile uint32_t *)CCM_CCGR_UART4_SET = 0x3333;
	/* UART4 パッドはリセット既定が ALT0=UART4 (m4/hello-world 実測) なので
	 * mux は不要。RX の入力 daisy だけ明示 (実測でも 2 だが依存しない) */
	*(volatile uint32_t *)0x3033050Cu = 0x2;	/* UART4_RXD input daisy */
	/* 前任 M4 が SPI 転送中に stop されていると ECSPI2 が動作状態のまま残り、
	 * SPI ドライバ init がハングする (state=running なのに無音、adc-test で実測)。
	 * M4 リセットはペリフェラルを初期化しないので毎起動 CONREG=0 で白紙に */
	*(volatile uint32_t *)0x30830008u = 0;
	return 0;
}
SYS_INIT(kart_m4_clocks_enable, PRE_KERNEL_1, 0);

#if CONFIG_IPM_MAX_DATA_SIZE > 0
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, d, s)
#else
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, NULL, 0)
#endif

#define SHM_NODE	DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR	DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE	DT_REG_SIZE(SHM_NODE)

/* ---- ワイヤ形式と Linux canid_t フラグ ---- */

struct kart_rpmsg_wire {
	uint32_t id;
	uint8_t dlc;
	uint8_t pad[3];
	uint8_t data[8];
} __packed;

#define LINUX_CAN_EFF_FLAG 0x80000000U
#define LINUX_CAN_RTR_FLAG 0x40000000U
#define LINUX_CAN_EFF_MASK 0x1FFFFFFFU
#define LINUX_CAN_SFF_MASK 0x000007FFU

/* ---- 制御メッセージ (kart-rpmsg-can.c と共通、8B。16B フレームと長さで区別) ----
 * Linux 側 candev の netlink 設定 (bitrate/mode) と up/down をここへ流す。
 * enum can_state は Zephyr と Linux で値が一致 (ACTIVE=0/WARN=1/PASSIVE=2/
 * BUS_OFF=3/STOPPED=4/SLEEPING=5) なので EVT_STATE は arg をそのまま渡す。 */
#define KART_RPMSG_CTRL_MAGIC 0xC7
enum {
	KART_RPMSG_CMD_SET_BITRATE = 1,	/* Linux→M4: arg = bitrate [bps] */
	KART_RPMSG_CMD_SET_MODE    = 2,	/* Linux→M4: flags = mode ビット */
	KART_RPMSG_CMD_START       = 3,	/* Linux→M4: CAN 起動 (ip link up) */
	KART_RPMSG_CMD_STOP        = 4,	/* Linux→M4: CAN 停止 (ip link down) */
	KART_RPMSG_EVT_STATE       = 5,	/* M4→Linux: arg = enum can_state */
};

struct kart_rpmsg_ctrl {
	uint8_t magic;
	uint8_t cmd;
	uint8_t flags;
	uint8_t rsvd;
	uint32_t arg;
} __packed;

#define KART_RPMSG_MODE_LISTENONLY (1u << 0)
#define KART_RPMSG_MODE_LOOPBACK   (1u << 1)

/* ---- CAN ---- */

static const struct device *const can_dev =
	DEVICE_DT_GET(DT_NODELABEL(can_mcp2515));

K_MSGQ_DEFINE(can_rx_q, sizeof(struct can_frame), 64, 4);

/* Linux の START/STOP で駆動する (旧: main() で無条件 can_start)。
 * SET_BITRATE/SET_MODE は停止中でないと適用できないので、必要なら stop する */
static bool can_started;

/* 状態変化 → EVT_STATE の遅延送出。CAN の状態変化コールバックは ISR/
 * ドライバ文脈で走り得るので、そこでは値を積むだけにし、main ループが送る */
static atomic_t pending_state = ATOMIC_INIT(-1);
static K_SEM_DEFINE(state_sem, 0, 1);

/* --- INT 可観測性: MCP2515 INT (GPIO3_IO24) のエッジをドライバとは独立の
 * callback で数え、IMR/PSR も直読みして stats に出す。gpio3 を Linux が
 * 掴んで IMR を消す時限バグ (kmm-yocto pitfalls #31) の切り分けで導入。
 * imr24 が 0 になったらこの類の再発 — stats 1 行で常時見張る */
static volatile uint32_t diag_edges;
static struct gpio_callback diag_int_cb;
static void diag_int_edge(const struct device *dev, struct gpio_callback *cb,
			  uint32_t pins)
{
	diag_edges++;
}
#define GPIO3_PSR (*(volatile uint32_t *)0x30220008u)
#define GPIO3_IMR (*(volatile uint32_t *)0x30220014u)

static void can_state_change_cb(const struct device *dev, enum can_state state,
				struct can_bus_err_cnt err_cnt, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(err_cnt);
	ARG_UNUSED(user_data);

	atomic_set(&pending_state, (atomic_val_t)state);
	k_sem_give(&state_sem);
}

/* 統計 (5 秒ごとに J64 へ) */
static atomic_t stat_can_rx;		/* CAN → rpmsg 転送数 */
static atomic_t stat_can_rx_drop;	/* msgq 満杯ドロップ */
static atomic_t stat_rpmsg_drop;	/* rpmsg TX バッファ枯渇ドロップ */
static atomic_t stat_can_tx;		/* rpmsg → CAN 送信数 */
static atomic_t stat_can_tx_drop;	/* can_send 失敗 */

/* ---- ADS8688 (8ch 16bit ADC、ECSPI2 の 2 個目の CS) ----
 *
 * raw SPI で叩く (Zephyr にドライバ無し)。手順・整列は ESP32 版 data-logger
 * (src/spi/ads8688.cpp) 準拠で apps/adc-test にて実機実証済み:
 *   RST → 全 ch range 0-5.12V → AUTO_SEQ_EN → AUTO_RST、
 *   読みは NO_OP×7 + 8 個目 AUTO_RST (毎周 ch0 巻き戻しで整列固定)。
 * 30Hz で 8ch を読み、CAN ID 0x700 (ch0-3) / 0x701 (ch4-7) に big-endian で
 * 詰めてバスへ送信し、同じフレームを can_rx_q にも積んで rpmsg 経由で
 * Linux にも見せる (受信フレームと同じ経路 = 追加コード無し)。
 * デコードは受信側 DBC の責務 (carrier-board-design.md)。 */
#define ADC_RATE_HZ	30

/* word=32bit: ADS8688 のフレームは全て 32bit (cmd16+data16 / addr8+data8+echo16)
 * に揃うので 1 転送 = 1 ワード = ISR 1 回で完結させる (8bit×4 の per-word
 * 方式より速く、ドライバの multi-word 経路のリスクも避けられる)。
 * 注意: GPIO CS + reg!=0 のデバイスは素の Zephyr ドライバではチャネル選択
 * 不整合で ISR が来ず全滅する — patches/zephyr/ の channel-select fix が必須 */
static const struct spi_dt_spec adc_spec = SPI_DT_SPEC_GET(
	DT_NODELABEL(adc_ads8688),
	SPI_OP_MODE_MASTER | SPI_MODE_CPHA | SPI_WORD_SET(32) | SPI_TRANSFER_MSB, 0);

static atomic_t stat_adc_tx;		/* ADC → CAN 送信数 (フレーム) */

static void adc_can_tx_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(error);
	ARG_UNUSED(user_data);
	/* fire-and-forget: 結果は使わない (失敗はバス側の問題で ADC は続行) */
}


static int ads8688_cmd(uint16_t cmd, uint16_t *out)
{
	/* 32bit word: 上位 16bit = コマンド、下位 16bit の応答が変換値 */
	uint32_t tx = (uint32_t)cmd << 16;
	uint32_t rx = 0;
	const struct spi_buf txb = { .buf = &tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = &rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int ret = spi_transceive_dt(&adc_spec, &txs, &rxs);

	if (ret == 0 && out != NULL) {
		*out = (uint16_t)(rx & 0xFFFF);
	}
	return ret;
}

static int ads8688_prog_write(uint8_t addr, uint8_t val)
{
	uint32_t tx = ((uint32_t)(((addr << 1) | 1)) << 24) | ((uint32_t)val << 16);
	const struct spi_buf txb = { .buf = &tx, .len = sizeof(tx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };

	return spi_write_dt(&adc_spec, &txs);
}


static int ads8688_init(void)
{
	int ret = ads8688_cmd(0x8500, NULL);	/* RST */

	if (ret != 0) {
		return ret;
	}
	k_msleep(1);
	for (int ch = 0; ch < 8; ch++) {
		ads8688_prog_write(0x05 + ch, 0x06);	/* 0..1.25*Vref = 0-5.12V */
	}
	ads8688_prog_write(0x01, 0xFF);		/* AUTO_SEQ_EN = 全 ch */
	return ads8688_cmd(0xA000, NULL);	/* AUTO_RST */
}

K_THREAD_STACK_DEFINE(thread_adc_stack, 1024);
static struct k_thread thread_adc_data;

static void adc_task(void *p1, void *p2, void *p3)
{
	uint16_t v[8];

	if (!spi_is_ready_dt(&adc_spec)) {
		printk("can-gw: ADS8688 SPI not ready — ADC 無効で継続\n");
		return;
	}


	int rc = ads8688_init();

	/* mcux ECSPI は configure (MasterInit) 直後の初回転送で ISR を落とすことが
	 * ある (実測 -116)。素直に間隔を置いてリトライする (FIFO 等へのロック外
	 * アクセスは全システム窒息を招くので厳禁 — 実測) */
	for (int retry = 0; rc != 0 && retry < 5; retry++) {
		printk("can-gw: ADS8688 init rc=%d — retry %d\n", rc, retry + 1);
		k_msleep(100);
		rc = ads8688_init();
	}
	if (rc != 0) {
		printk("can-gw: ADS8688 init rc=%d — ADC 無効で継続\n", rc);
		return;
	}
	printk("can-gw: ADS8688 sampling %dHz -> CAN 0x%03x/0x%03x\n",
	       ADC_RATE_HZ, KART_CAN_DL_700_FRAME_ID, KART_CAN_DL_701_FRAME_ID);

	while (1) {
		for (int ch = 0; ch < 8; ch++) {
			uint16_t cmd = (ch == 7) ? 0xA000 : 0x0000;

			if (ads8688_cmd(cmd, &v[ch]) != 0) {
				v[ch] = 0xFFFF;
			}
		}
		/* フレームレイアウトは kart-can (can.yaml) の生成 pack に一任。
		 * v[] は生カウントで、生成構造体のフィールドも生カウント (raw) */
		const struct kart_can_dl_700_t m700 = {
			.dl_adc_ch0 = v[0], .dl_adc_ch1 = v[1],
			.dl_adc_ch2 = v[2], .dl_adc_ch3 = v[3],
		};
		const struct kart_can_dl_701_t m701 = {
			.dl_adc_ch4 = v[4], .dl_adc_ch5 = v[5],
			.dl_adc_ch6 = v[6], .dl_adc_ch7 = v[7],
		};

		for (int f = 0; f < 2; f++) {
			struct can_frame frame = {
				.id = (f == 0) ? KART_CAN_DL_700_FRAME_ID
					       : KART_CAN_DL_701_FRAME_ID,
				.dlc = 8,
			};

			if (f == 0) {
				kart_can_dl_700_pack(frame.data, &m700,
						     sizeof(frame.data));
			} else {
				kart_can_dl_701_pack(frame.data, &m701,
						     sizeof(frame.data));
			}
			/* rpmsg へ (受信フレームと同じ経路に相乗り)。can_send より
			 * 先に行う — バスに ACK 相手が居ないと同期 can_send は
			 * 返ってこないため (実測でここで永久ブロックした) */
			k_msgq_put(&can_rx_q, &frame, K_NO_WAIT);
			/* バスへは fire-and-forget (callback 付き非同期)。ACK 不在や
			 * error passive でも adc_task を塞がない */
			/* K_MSEC(5) は「TX バッファ空き待ち」のみ (callback 方式なので
			 * 送信完了は待たない = ACK 不在でもブロックしない)。Zephyr の
			 * mcp2515 ドライバは TX を 1 本しか使わない (MCP2515_TX_CNT=1)
			 * ため、K_NO_WAIT だと 0x700 の in-flight 中 (~108us@1Mbps) に
			 * 投げる 0x701 が毎回 -EAGAIN で消えていた (2026-08-24 実測) */
			if (can_started &&
			    can_send(can_dev, &frame, K_MSEC(5),
				     adc_can_tx_cb, NULL) == 0) {
				atomic_inc(&stat_adc_tx);
			}
		}
		k_sleep(K_USEC(1000000 / ADC_RATE_HZ));
	}
}

static void can_rx_cb(const struct device *dev, struct can_frame *frame,
		      void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (k_msgq_put(&can_rx_q, frame, K_NO_WAIT) != 0) {
		atomic_inc(&stat_can_rx_drop);
	}
}

/* ---- rpmsg (openamp_rsc_table 構成) ---- */

K_THREAD_STACK_DEFINE(thread_mng_stack, 1024);
K_THREAD_STACK_DEFINE(thread_gw_stack, 1024);
static struct k_thread thread_mng_data;
static struct k_thread thread_gw_data;

static const struct device *const ipm_handle =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_tab_physmap;
static struct metal_io_region shm_io_data;
static struct metal_io_region rsc_io_data;
static struct metal_io_region *shm_io = &shm_io_data;
static struct metal_io_region *rsc_io = &rsc_io_data;
static struct rpmsg_virtio_device rvdev;
static void *rsc_table;
static struct rpmsg_device *rpdev;

static struct rpmsg_endpoint can_ept;
/* Linux 側 ept アドレス。probe 時の "hi" (または任意の受信) で学習する */
static atomic_t peer_addr = ATOMIC_INIT(-1);

static K_SEM_DEFINE(data_sem, 0, 1);
static K_SEM_DEFINE(rpdev_ready_sem, 0, 1);

static void platform_ipm_callback(const struct device *dev, void *context,
				  uint32_t id, volatile void *data)
{
	k_sem_give(&data_sem);
}

/* Linux (candev) からの制御メッセージを CAN コントローラへ反映する。
 * mng スレッド文脈から呼ばれる (CAN API 呼び出しは可)。設定は ip link up 時の
 * 数発だけなので、ここで can_stop/start しても vring 処理への影響は軽微 */
static void kart_rpmsg_handle_ctrl(const struct kart_rpmsg_ctrl *c)
{
	int ret;

	switch (c->cmd) {
	case KART_RPMSG_CMD_SET_BITRATE:
		if (can_started) {
			can_stop(can_dev);
			can_started = false;
		}
		ret = can_set_bitrate(can_dev, c->arg);
		printk("can-gw: set_bitrate %u rc=%d\n", c->arg, ret);
		break;
	case KART_RPMSG_CMD_SET_MODE: {
		can_mode_t mode = CAN_MODE_NORMAL;

		if (c->flags & KART_RPMSG_MODE_LISTENONLY) {
			mode |= CAN_MODE_LISTENONLY;
		}
		if (c->flags & KART_RPMSG_MODE_LOOPBACK) {
			mode |= CAN_MODE_LOOPBACK;
		}
		if (can_started) {
			can_stop(can_dev);
			can_started = false;
		}
		ret = can_set_mode(can_dev, mode);
		printk("can-gw: set_mode 0x%x rc=%d\n", (unsigned int)mode, ret);
		break;
	}
	case KART_RPMSG_CMD_START:
		if (!can_started) {
			ret = can_start(can_dev);
			can_started = (ret == 0);
			printk("can-gw: start rc=%d\n", ret);
		}
		break;
	case KART_RPMSG_CMD_STOP:
		if (can_started) {
			can_stop(can_dev);
			can_started = false;
			printk("can-gw: stop\n");
		}
		break;
	default:
		break;
	}
}

static int rpmsg_recv_can_callback(struct rpmsg_endpoint *ept, void *data,
				   size_t len, uint32_t src, void *priv)
{
	struct kart_rpmsg_wire w;
	struct can_frame frame = {0};
	int ret;

	atomic_set(&peer_addr, (atomic_val_t)src);

	if (len == sizeof(struct kart_rpmsg_ctrl)) {	/* 制御メッセージ (8B) */
		const struct kart_rpmsg_ctrl *c = data;

		if (c->magic == KART_RPMSG_CTRL_MAGIC) {
			kart_rpmsg_handle_ctrl(c);
		}
		return RPMSG_SUCCESS;
	}
	if (len != sizeof(w)) {	/* "hi" ハンドシェイク等 */
		return RPMSG_SUCCESS;
	}
	memcpy(&w, data, sizeof(w));

	if (w.id & LINUX_CAN_EFF_FLAG) {
		frame.id = w.id & LINUX_CAN_EFF_MASK;
		frame.flags |= CAN_FRAME_IDE;
	} else {
		frame.id = w.id & LINUX_CAN_SFF_MASK;
	}
	if (w.id & LINUX_CAN_RTR_FLAG) {
		frame.flags |= CAN_FRAME_RTR;
	}
	frame.dlc = MIN(w.dlc, CAN_MAX_DLC);
	memcpy(frame.data, w.data, can_dlc_to_bytes(frame.dlc));

	/* mng スレッド文脈。MCP2515 の TX メールボックス待ちはしない —
	 * 満杯なら vring 処理を止めないためドロップ */
	ret = can_send(can_dev, &frame, K_NO_WAIT, NULL, NULL);
	if (ret == 0) {
		atomic_inc(&stat_can_tx);
	} else {
		atomic_inc(&stat_can_tx_drop);
	}
	return RPMSG_SUCCESS;
}

static void new_service_cb(struct rpmsg_device *rdev, const char *name,
			   uint32_t src)
{
	LOG_ERR("%s: unexpected ns service receive for name %s",
		__func__, name);
}

int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);

	/* kart: Linux 側 (imx_rproc) の mboxes は MU レジスタ index 1 固定
	 * (<&mu 1 1>)。OpenAMP が渡してくる id (= カーネルが採番した
	 * notifyid 0/1) をチャネル番号に使うと TR[0] 行きになり Linux に
	 * 届かない (実測 — MU 割込ゼロ)。チャネルは 1 に固定し、id は
	 * データとして載せる (imx_rproc は値を見ず全 vq を叩くので可) */
	IPM_SEND(ipm_handle, 0, 1, &id, 4);

	return 0;
}

static int platform_init(void)
{
	int rsc_size;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	int status;

	status = metal_init(&metal_params);
	if (status) {
		LOG_ERR("metal_init: failed: %d", status);
		return -1;
	}

	metal_io_init(shm_io, (void *)SHM_START_ADDR, &shm_physmap,
		      SHM_SIZE, -1, 0, addr_translation_get_ops(shm_physmap));

	void *linked_rsc = NULL;

	rsc_table_get(&linked_rsc, &rsc_size);

	/* kart: imx_rproc は「ライブテーブル」を Linux DT の rsc-table 予約
	 * 領域 (0xB80FF000) に置く (ELF から解析したテーブルをそこへコピーし、
	 * status=DRIVER_OK と vring 実アドレスの書き戻しもそこに行く)。
	 * イメージ内のテーブルを読むと da=-1/status=0 のままで永遠に待つ
	 * (実測)。リンク済みテーブルは ELF 解析用に残しつつ、実行時は
	 * 共有側 (0xB80FF000) を参照する。
	 *
	 * [attach 対応] 起動経路が 2 通り:
	 *  - remoteproc LOAD (Linux が M4 を起動): Linux が ELF を解析して
	 *    0xB80FF000 にテーブルを書いてから M4 の reset を解除する。M4 が
	 *    走り出す時点で ver==1 の有効テーブルが既にある。
	 *  - SPL loadable / attach (M4 が Linux より先住): Linux は ELF を
	 *    解析せず 0xB80FF000 を「読む」側に回る。誰も書かないので M4 自身が
	 *    リンク済みテーブルをここへ publish する必要がある (実測: 現状は
	 *    M4 が書かないため attach では Linux が空テーブルを読む)。
	 * 判定: 先頭 u32 = テーブル version。有効 (==1) なら Linux が既に書いた
	 * (LOAD) ので上書きしない (Linux が書き戻す vring da を潰さないため)。
	 * 無効なら attach なので M4 が publish する。 */
	rsc_table = (void *)0xB80FF000UL;
	if (*(volatile uint32_t *)rsc_table != 1u) {
		memcpy(rsc_table, linked_rsc, rsc_size);
		LOG_INF("rsc_table published to 0xB80FF000 (attach mode)");
	}
	rsc_tab_physmap = (uintptr_t)rsc_table;

	metal_io_init(rsc_io, rsc_table,
		      &rsc_tab_physmap, rsc_size, -1, 0, NULL);

	if (!device_is_ready(ipm_handle)) {
		LOG_ERR("IPM device is not ready");
		return -1;
	}

	/* [MU 衝突回避] MU 受信割込みは有効化しない。実測 (kmm-yocto pitfalls
	 * #26 追記): RDC+CCGR を満たしても「MU doorbell 受信 × ペリフェラル
	 * read」の同時進行で SoC がハードリセットする (ベアメタル再現済み)。
	 * 受信は mng タスクの 1ms ポーリングに置き換え、M4 に MU 割込みが
	 * 入らないようにする (M4→Linux 方向の kick は 2 万/s でも安全を実測)。 */
	ipm_register_callback(ipm_handle, platform_ipm_callback, NULL);

	return 0;
}

static struct rpmsg_device *
platform_create_rpmsg_vdev(void (*rst_cb)(struct virtio_device *vdev),
			   rpmsg_ns_bind_cb ns_cb)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device *vdev;
	int ret;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
					rsc_table_to_vdev(rsc_table),
					rsc_io, NULL, mailbox_notify, NULL);
	if (!vdev) {
		LOG_ERR("failed to create vdev");
		return NULL;
	}

	/* Linux 側の rpmsg 初期化完了 (status=DRIVER_OK) を待つ */
	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		LOG_ERR("failed to init vring 0");
		goto failed;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		LOG_ERR("failed to init vring 1");
		goto failed;
	}

	ret = rpmsg_init_vdev(&rvdev, vdev, ns_cb, shm_io, NULL);
	if (ret) {
		LOG_ERR("failed rpmsg_init_vdev");
		goto failed;
	}

	/* [MU 衝突回避・核心] used ring に VRING_USED_F_NO_NOTIFY を立て、
	 * Linux (driver 側) の kick = MU write を仕様レベルで止める。
	 * 実測 (kmm-yocto pitfalls #26): RDC+CCGR を満たしても「Linux の
	 * MU write × M4 のペリフェラル read」の同時進行で SoC がハード
	 * リセットする。M4 側は 1ms ポーリング受信なので kick は不要 */
	rvdev.rvq->vq_ring.used->flags |= VRING_USED_F_NO_NOTIFY;
	rvdev.svq->vq_ring.used->flags |= VRING_USED_F_NO_NOTIFY;

	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

static void rpmsg_mng_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (platform_init()) {
		LOG_ERR("Failed to initialize platform");
		return;
	}

	rpdev = platform_create_rpmsg_vdev(NULL, new_service_cb);
	if (!rpdev) {
		LOG_ERR("Failed to create rpmsg virtio device");
		return;
	}

	k_sem_give(&rpdev_ready_sem);

	while (1) {
		/* MU 割込み無し運用: 1ms 周期で vring を見る (notified は
		 * 仕事が無ければ即戻る)。CAN GW 用途にはレイテンシ十分 */
		k_sem_take(&data_sem, K_MSEC(1));
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}
}

static void can_gw_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct can_frame frame;
	struct kart_rpmsg_wire w;
	int ret;

	k_sem_take(&rpdev_ready_sem, K_FOREVER);

	/* NS 告知 → Linux 側で kart-rpmsg-can が probe → rpcan0 登録 */
	ret = rpmsg_create_ept(&can_ept, rpdev, "kart-can",
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       rpmsg_recv_can_callback, NULL);
	if (ret) {
		LOG_ERR("could not create kart-can endpoint: %d", ret);
		return;
	}
	printk("can-gw: kart-can ept created, waiting for Linux peer\n");

	while (1) {
		k_msgq_get(&can_rx_q, &frame, K_FOREVER);

		uint32_t peer = (uint32_t)atomic_get(&peer_addr);

		if (peer == (uint32_t)-1) {
			continue;	/* Linux 側まだ — 捨てる */
		}

		memset(&w, 0, sizeof(w));
		w.id = frame.id;
		if (frame.flags & CAN_FRAME_IDE) {
			w.id = (frame.id & LINUX_CAN_EFF_MASK) |
			       LINUX_CAN_EFF_FLAG;
		} else {
			w.id = frame.id & LINUX_CAN_SFF_MASK;
		}
		if (frame.flags & CAN_FRAME_RTR) {
			w.id |= LINUX_CAN_RTR_FLAG;
		}
		w.dlc = MIN(frame.dlc, CAN_MAX_DLC);
		memcpy(w.data, frame.data, can_dlc_to_bytes(w.dlc));

		/* 非ブロック — TX バッファ枯渇 (Linux 停滞) はドロップ */
		ret = rpmsg_trysendto(&can_ept, &w, sizeof(w), peer);
		if (ret == 0 || ret == sizeof(w)) {
			atomic_inc(&stat_can_rx);
		} else {
			atomic_inc(&stat_rpmsg_drop);
		}
	}
}

int main(void)
{
	const struct can_filter filter_all = {
		.flags = 0,
		.id = 0,
		.mask = 0,	/* mask 0 = 全 ID */
	};
	int ret;

	printk("kart M4: can-gw (MCP2515 @ ECSPI2 1Mbps <-> rpmsg kart-can)\n");

	if (!device_is_ready(can_dev)) {
		printk("CAN device NOT ready\n");
		return 0;
	}
	/* can_start は Linux (candev) の START = `ip link up` 契機で行う
	 * (SET_BITRATE/SET_MODE を停止中に受けてから起動するため)。ここでは
	 * 全 ID フィルタ登録と状態変化コールバック登録だけ */
	ret = can_add_rx_filter(can_dev, can_rx_cb, NULL, &filter_all);
	printk("add_rx_filter rc=%d\n", ret);
	can_set_state_change_callback(can_dev, can_state_change_cb, NULL);

	k_thread_create(&thread_mng_data, thread_mng_stack,
			K_THREAD_STACK_SIZEOF(thread_mng_stack),
			rpmsg_mng_task, NULL, NULL, NULL,
			K_PRIO_COOP(8), 0, K_NO_WAIT);
	k_thread_create(&thread_gw_data, thread_gw_stack,
			K_THREAD_STACK_SIZEOF(thread_gw_stack),
			can_gw_task, NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	/* ADC は CAN/rpmsg の状態と独立に回す (gw より低優先) */
	{
		const struct device *g3 = DEVICE_DT_GET(DT_NODELABEL(gpio3));
		gpio_init_callback(&diag_int_cb, diag_int_edge, BIT(24));
		gpio_add_callback(g3, &diag_int_cb);
	}

	k_thread_create(&thread_adc_data, thread_adc_stack,
			K_THREAD_STACK_SIZEOF(thread_adc_stack),
			adc_task, NULL, NULL, NULL,
			K_PRIO_COOP(6), 0, K_NO_WAIT);

	while (1) {
		enum can_state state;
		struct can_bus_err_cnt errs;

		/* 状態変化 (state_sem) で即起きて EVT_STATE を Linux へ。無ければ
		 * 5s ごとに J64 へ統計を出す */
		if (k_sem_take(&state_sem, K_SECONDS(5)) == 0) {
			atomic_val_t st = atomic_get(&pending_state);
			uint32_t peer = (uint32_t)atomic_get(&peer_addr);

			if (st >= 0 && peer != (uint32_t)-1) {
				struct kart_rpmsg_ctrl e = {
					.magic = KART_RPMSG_CTRL_MAGIC,
					.cmd = KART_RPMSG_EVT_STATE,
					.arg = (uint32_t)st,
				};

				rpmsg_trysendto(&can_ept, &e, sizeof(e), peer);
				printk("can-gw: EVT_STATE %ld -> Linux\n", (long)st);
			}
			continue;	/* 統計は次の timeout 周で */
		}

		can_get_state(can_dev, &state, &errs);
		printk("--- can->rp %ld (qdrop %ld, rpdrop %ld) | rp->can %ld (drop %ld) | adc->can %ld | peer 0x%x | started=%d state=%d tx_err=%d rx_err=%d ---\n",
		       atomic_get(&stat_can_rx),
		       atomic_get(&stat_can_rx_drop),
		       atomic_get(&stat_rpmsg_drop),
		       atomic_get(&stat_can_tx),
		       atomic_get(&stat_can_tx_drop),
		       atomic_get(&stat_adc_tx),
		       (uint32_t)atomic_get(&peer_addr),
		       can_started,
		       state, errs.tx_err_cnt, errs.rx_err_cnt);
		printk("    int-diag: edges=%u lvl=%d imr24=%d\n",
		       diag_edges,
		       (int)((GPIO3_PSR >> 24) & 1),
		       (int)((GPIO3_IMR >> 24) & 1));
	}
	return 0;
}
