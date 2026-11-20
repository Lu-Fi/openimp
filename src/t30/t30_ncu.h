/*
 * T30 NCU (noise cancel unit, 3D denoise engine at 0x13090000) driven from user space
 * like the vendor libimp (Lu-Fi/openimp #701, docs/T30_NCU.md).
 */
#ifndef OPENIMP_T30_NCU_H
#define OPENIMP_T30_NCU_H

#include <stdint.h>

#define T30_NCU_REG_BASE 0x13090000u
#define T30_NCU_BIN104_SIZE 0x1548u /* vendor 1.0.4 ncu_load_param fread size */
#define T30_NCU_SET104_SIZE 0xaa0u  /* one day / night parameter set */

/* /etc/sensor/ncu_<sensor>.bin layouts */
enum t30_ncu_bin_kind {
    T30_NCU_BIN_NONE = 0,    /* no file: Temper (vendor "try to use temper") */
    T30_NCU_BIN_104 = 1,     /* 0x1548 bytes, word0 = size: vendor 1.0.4 layout, supported */
    T30_NCU_BIN_105 = 2,     /* 0xec8 bytes, version byte 1: vendor 1.0.5 layout, not supported */
    T30_NCU_BIN_INVALID = 3, /* unreadable or unknown layout */
};

/* Classify the bin; for T30_NCU_BIN_104 *mode gets the vendor 3D mode ((word1 >> 16) & 3):
 * 0 NCU always on, 1 Temper by day / NCU at night, 2 Temper only. */
int t30_ncu_bin_probe(const char *path, uint32_t *mode);

/* vendor ncu_hal_init(sensor, w, h): stride/size setup, parameter load, first download */
int t30_ncu_hal_init(const char *sensor, int w, int h);
/* vendor ncu_hal_buf_size (mode 0 buffer) and ncu_hal_buf_init(virt, phys, size) */
uint32_t t30_ncu_hal_buf_size(void);
int t30_ncu_hal_buf_init(uint32_t virt, uint32_t phys, uint32_t size);
/* vendor ncu_ctrl_bypass / ncu_ctrl_update_mode / ncu_ctrl_update */
void t30_ncu_ctrl_bypass(int bypass);
void t30_ncu_ctrl_update_mode(int night);
void t30_ncu_ctrl_update(int32_t total_gain);
/* vendor g_total_gain (isp_tuning_func_update_total_gain), read by ncu_ctrl_update_param */
void t30_ncu_set_total_gain(int32_t total_gain);
/* stop the software updates (DisableSensor frees the buffer) */
void t30_ncu_deinit(void);
int t30_ncu_active(void);

/* physical register access like the vendor write_reg_32 / read_reg_32 (/dev/mem per access) */
int t30_phys_write32(uint32_t addr, uint32_t val);
int t30_phys_read32(uint32_t addr, uint32_t *val);

/* register backend; NULL restores /dev/mem (tests install a fake) */
typedef void (*t30_ncu_write_fn)(uint32_t addr, uint32_t val);
typedef uint32_t (*t30_ncu_read_fn)(uint32_t addr);
void t30_ncu_set_reg_ops(t30_ncu_write_fn wr, t30_ncu_read_fn rd);
/* directory of ncu_<sensor>.bin (default /etc/sensor; tests only) */
void t30_ncu_set_bin_dir(const char *dir);

#endif
