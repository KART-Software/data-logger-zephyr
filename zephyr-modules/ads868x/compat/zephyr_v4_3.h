/*
 * Zephyr v4.3.0 向け互換シム。ドライバ本体は upstream main の API 名で書いてあり
 * (upstream PR にそのまま出すため)、古い Zephyr に無い名前だけここで埋める。
 * CMakeLists.txt が -include で全 TU の先頭に差し込む。upstream には持ち込まない。
 */
#ifndef ADS868X_COMPAT_ZEPHYR_V4_3_H
#define ADS868X_COMPAT_ZEPHYR_V4_3_H

/* SPI_OP_MODE_MASTER → SPI_OP_MODE_CONTROLLER への改名 (v4.3 より後) */
#ifndef SPI_OP_MODE_CONTROLLER
#define SPI_OP_MODE_CONTROLLER SPI_OP_MODE_MASTER
#endif

#endif /* ADS868X_COMPAT_ZEPHYR_V4_3_H */
