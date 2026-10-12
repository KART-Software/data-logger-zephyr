/*
 * adc-dt — ADS868x ドライバ (zephyr-modules/ads868x) の実機確認。
 *
 * Zephyr の samples/drivers/adc/adc_dt と同じ流儀で DT の zephyr,user io-channels を
 * 標準 ADC API で読む。加えて全チャネルを 1 シーケンス (channels マスク) で読み、
 * 1ch ずつの読みと一致することを確認する (ドライバのパイプライン読みの検証)。
 * bipolar チャネルは符号付き 16bit として扱う。
 */
#include <inttypes.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)
};

static int32_t to_value(const struct adc_dt_spec *spec, uint16_t raw)
{
	return spec->channel_cfg.differential ? (int32_t)(int16_t)raw : (int32_t)raw;
}

int main(void)
{
	uint16_t buf;
	uint16_t all[ARRAY_SIZE(adc_channels)];
	struct adc_sequence sequence = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};
	struct adc_sequence all_seq = {
		.channels = 0,
		.buffer = all,
		.buffer_size = sizeof(all),
		.resolution = 16,
	};
	uint32_t count = 0;
	int err;

	printk("adc-dt: %u channels\n", (unsigned int)ARRAY_SIZE(adc_channels));

	for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
		if (!adc_is_ready_dt(&adc_channels[i])) {
			printk("ADC controller device %s not ready\n", adc_channels[i].dev->name);
			return 0;
		}
		err = adc_channel_setup_dt(&adc_channels[i]);
		if (err < 0) {
			printk("Could not setup channel #%u (%d)\n", (unsigned int)i, err);
			return 0;
		}
		all_seq.channels |= BIT(adc_channels[i].channel_id);
	}

	while (1) {
		printk("ADC reading[%u]:\n", count++);
		for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
			int32_t val_mv;

			(void)adc_sequence_init_dt(&adc_channels[i], &sequence);
			err = adc_read_dt(&adc_channels[i], &sequence);
			if (err < 0) {
				printk("- ch%d: read failed (%d)\n", adc_channels[i].channel_id, err);
				continue;
			}
			val_mv = to_value(&adc_channels[i], buf);
			printk("- ch%d: raw=0x%04x %" PRId32, adc_channels[i].channel_id, buf, val_mv);
			err = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
			if (err < 0) {
				printk(" (mV n/a)\n");
			} else {
				printk(" = %" PRId32 " mV\n", val_mv);
			}
		}

		/* 全チャネルを 1 シーケンスで (順序は channel 番号順) */
		err = adc_read(adc_channels[0].dev, &all_seq);
		if (err < 0) {
			printk("- all: read failed (%d)\n", err);
		} else {
			printk("- all:");
			for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
				printk(" 0x%04x", all[i]);
			}
			printk("\n");
		}

		k_sleep(K_MSEC(1000));
	}
	return 0;
}
