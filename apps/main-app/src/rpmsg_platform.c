/*
 * rpmsg_platform — Linux (imx_rproc) との rpmsg を立ち上げる、main-app の土台。
 * can-gw の rpmsg 部 (apps/rpmsg-echo = openamp_rsc_table サンプルと同じ構成) を移したもの。
 * imx_rproc との噛み合わせ (ライブリソーステーブル / attach / MU チャネル 1 固定 /
 * MU 割込みを使わない 1ms ポーリング) は、各所のコメントのとおり。
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <string.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>
#include <addr_translation.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(rpmsg_platform);

#include "rpmsg_platform.h"

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "main-app requires definition of shared memory for rpmsg"
#endif

#if CONFIG_IPM_MAX_DATA_SIZE > 0
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, d, s)
#else
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, NULL, 0)
#endif

#define SHM_NODE	DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR	DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE	DT_REG_SIZE(SHM_NODE)

/* imx_rproc のライブテーブル先 (Linux DT の rsc-table 予約領域)。
 * 8MM: 0xB80FF000 / 8MP: 0x550FF000 (kmm-yocto docs/imx8mp-debix-bringup/01-m7.md) */
#if defined(CONFIG_SOC_MIMX8ML8)
#define RSC_TABLE_ADDR	0x550FF000UL
#else
#define RSC_TABLE_ADDR	0xB80FF000UL
#endif

K_THREAD_STACK_DEFINE(thread_mng_stack, 1024);
static struct k_thread thread_mng_data;

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

static K_SEM_DEFINE(data_sem, 0, 1);
/* 準備ができたら (失敗でも) 1 回 give する。待つ側は take してすぐ give し直す */
static K_SEM_DEFINE(ready_sem, 0, 1);

static void platform_ipm_callback(const struct device *dev, void *context,
				  uint32_t id, volatile void *data)
{
	k_sem_give(&data_sem);
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

	/* Linux 側 (imx_rproc) の mboxes は MU レジスタ index 1 固定
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

	/* imx_rproc は「ライブテーブル」を Linux DT の rsc-table 予約領域
	 * (RSC_TABLE_ADDR) に置く。イメージ内のテーブルを読むと da=-1/status=0 の
	 * ままで永遠に待つ (実測)。リンク済みテーブルは ELF 解析用に残しつつ、
	 * 実行時は共有側を参照する。
	 *
	 * 起動経路が 2 通り:
	 *  - remoteproc LOAD (Linux が M コアを起動): Linux が ELF を解析して
	 *    テーブルを書いてから reset を解除する。走り出す時点で ver==1 の
	 *    有効テーブルが既にある。
	 *  - SPL/BL31 で先に起動 (attach): Linux は ELF を解析せず、テーブルを
	 *    「読む」側に回る。誰も書かないので M コア自身が publish する。
	 * 判定: 先頭 u32 = テーブル version。有効 (==1) なら Linux が既に書いた
	 * (LOAD) ので上書きしない (Linux が書き戻す vring da を潰さないため)。 */
	rsc_table = (void *)RSC_TABLE_ADDR;
	if (*(volatile uint32_t *)rsc_table != 1u) {
		memcpy(rsc_table, linked_rsc, rsc_size);
		LOG_INF("rsc_table published to %lx (attach mode)",
			(unsigned long)RSC_TABLE_ADDR);
	}
	rsc_tab_physmap = (uintptr_t)rsc_table;

	metal_io_init(rsc_io, rsc_table,
		      &rsc_tab_physmap, rsc_size, -1, 0, NULL);

	if (!device_is_ready(ipm_handle)) {
		LOG_ERR("IPM device is not ready");
		return -1;
	}

	/* [MU 衝突回避] MU 受信割込みは有効化しない。実測 (kmm-yocto pitfalls
	 * #26): 「MU doorbell 受信 × ペリフェラル read」の同時進行で SoC が
	 * ハードリセットする。受信は管理スレッドの 1ms ポーリングに置き換える。 */
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
	 * Linux の kick = MU write を仕様レベルで止める (kmm-yocto pitfalls #26)。
	 * M コア側は 1ms ポーリング受信なので kick は不要 */
	rvdev.rvq->vq_ring.used->flags |= VRING_USED_F_NO_NOTIFY;
	rvdev.svq->vq_ring.used->flags |= VRING_USED_F_NO_NOTIFY;

	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

#if defined(CONFIG_SOC_MIMX8ML8)
/* Linux (imx_rproc) の kick = MU-A TRn 書き込みは、M7 側が MU-B RRn を読むまで
 * 完了しない (Linux は tx_tout=100ms で待ち、err -62 で諦める)。attach 直後の
 * 1 発がこれで 100ms 止まり、PID1 が遅れていた。MU 割込みは使わないまま、
 * 1ms ポーリングの中で受信フルフラグを見て読み捨てる = ACK だけ返す。
 * MU-B: 0x30ab0000 (nxp_imx8ml_m7.dtsi の mailbox0)。SR bit27-n = RFn */
#define MUB_SR		(*(volatile uint32_t *)0x30ab0020u)
#define MUB_RR(n)	(*(volatile uint32_t *)(0x30ab0010u + 4u * (n)))
static inline void mu_ack_rx(void)
{
	uint32_t sr = MUB_SR;

	for (unsigned int n = 0; n < 4; n++) {
		if (sr & (1u << (27 - n))) {
			(void)MUB_RR(n);
		}
	}
}
#else
static inline void mu_ack_rx(void) {}
#endif

static void rpmsg_mng_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (platform_init()) {
		LOG_ERR("Failed to initialize platform");
		k_sem_give(&ready_sem);
		return;
	}

	rpdev = platform_create_rpmsg_vdev(NULL, new_service_cb);
	if (!rpdev) {
		LOG_ERR("Failed to create rpmsg virtio device");
	}
	k_sem_give(&ready_sem);
	if (!rpdev) {
		return;
	}

	while (1) {
		/* MU 割込み無し運用: 1ms 周期で vring を見る (仕事が無ければ即戻る) */
		k_sem_take(&data_sem, K_MSEC(1));
		mu_ack_rx();
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}
}

void rpmsg_platform_start(void)
{
	k_thread_create(&thread_mng_data, thread_mng_stack,
			K_THREAD_STACK_SIZEOF(thread_mng_stack),
			rpmsg_mng_task, NULL, NULL, NULL,
			K_PRIO_COOP(8), 0, K_NO_WAIT);
}

struct rpmsg_device *rpmsg_platform_wait(void)
{
	k_sem_take(&ready_sem, K_FOREVER);
	k_sem_give(&ready_sem);
	return rpdev;
}
