/*
 * OpenIMP T32 kernel ioctl ABI (#287, planned; not a release gate).
 *
 * Every request number and payload size that OpenIMP T32 (PRJ007, vendor
 * 2.x kernel ABI) talks to: /dev/tx-isp, /dev/isp-m0, /dev/misc-ivdc,
 * /dev/framechanN (all type 'T') and the kernel's /dev/soc_vpu (type 'c').
 * Bases: DEVICE 0, TUNING 100, IVDC 110, FS 130, LDC 180, DEVICE1 200.
 * The T32 numbers differ from T41 (same names, other numbers or sizes):
 * never use the T40/T41 constants of src/t40 on this SoC.
 *
 * Sources, all observations (no vendor code copied):
 *  - the requests vendor libimp 2.2.0 issues in QEMU -M ingenic-t32
 *    (flag OI_T32_F_SEEN; reference sequence tests/t32/vendor-2.2.0-ioctl-seq.txt,
 *    recorder tools/t32/qemu-ioctl-trace.sh);
 *  - the open-tx-isp driver/t32 table tx_isp_t32_abi.h (flag OI_T32_F_DRV:
 *    the open driver serves it; cross-checked by tests/t32 "check-drv");
 *  - the number/size of the remaining 2.x requests (no flag: known, not
 *    used yet by libimp in the reference flows nor served by the driver).
 *
 * MIPS ioctl encoding: dir bits 31:29 (1 none, 2 read, 4 write, 6 both),
 * size 28:16, type 15:8, nr 7:0.  Three requests carry a buffer larger
 * than their size field (DRIVER_VERSION 64 B, ENUMINPUT 36 B, DEFAULT_BIN
 * 880 B): the table's payload column is the real buffer.
 *
 * Each literal below is asserted against OI_T32_IOC(dir, type, nr,
 * sizeof(payload struct)), so a struct size change breaks the build.
 */
#ifndef OPENIMP_T32_IOCTL_H
#define OPENIMP_T32_IOCTL_H

#include <stdint.h>

#define OI_T32_DIR_NONE 1u
#define OI_T32_DIR_R    2u
#define OI_T32_DIR_W    4u
#define OI_T32_DIR_RW   6u
#define OI_T32_IOC(dir, type, nr, size) \
    (((uint32_t)(dir) << 29) | ((uint32_t)(size) << 16) | \
     ((uint32_t)(unsigned char)(type) << 8) | (uint32_t)(nr))
#define OI_T32_IOC_SIZE(req) (((uint32_t)(req) >> 16) & 0x1fffu)
#define OI_T32_IOC_NR(req)   ((uint32_t)(req) & 0xffu)

/* ---- payloads (mipsel o32; fixed-width so the host test sees the same) -- */
typedef struct {                /* G_INPUT, S_INPUT, STREAMON/OFF, LINKS_* */
    int32_t enable;
    int32_t vinum;
} T32IocInitArg;

typedef struct {                /* *_MEM_INFO, MEMORY_MALLOC/FREE */
    uint32_t word0;             /* vinum (mem info) / kva (malloc out, free in) */
    uint32_t paddr;
    uint32_t size;
} T32IocBufInfo;

typedef struct {                /* SET/GET_SENSOR_REGISTER */
    char name[32];
    int32_t sensor_id;
    int32_t type;
    uint32_t size;
    uint32_t pad;               /* u64 alignment on o32 */
    uint32_t reg[2];            /* u64 */
    uint32_t val[2];            /* u64 */
} T32IocDbgRegister;

typedef struct {                /* REGISTER/RELEASE_SENSOR (= IMPSensorInfo) */
    uint8_t bytes[116];
} T32IocSensorReg;

typedef struct {                /* DRIVER_VERSION (size field 4) */
    char soc[32];               /* "PRJ007" */
    char version[32];
} T32IocDriverVersion;

typedef struct {                /* ENUMINPUT (size field 4) */
    int32_t index;
    char name[32];
} T32IocEnumInput;

typedef struct {                /* DEFAULT_BIN (size field 4) */
    uint8_t bytes[880];
} T32IocDefaultBin;

/* SET_TIZIANO_PAR (size field 4; the vendor driver copies 20 B, #289):
 * the loaded IQ bin = vendor IMPISPSwitchBinAttr */
typedef struct {
    uint32_t paddr;             /* MEMORY_MALLOC buffer holding the bin */
    uint32_t size;              /* bin file size */
    uint32_t wdr;               /* 1 = WDR bin (day-wdr / night-wdr) */
    uint32_t night;             /* 1 = night bin */
    uint8_t update_csccr;       /* 1 when day and night changed */
    uint8_t vinum;
    uint8_t pad[2];
} T32IocTizianoPar;

typedef struct {                /* S_CTRL / G_CTRL */
    uint32_t id;
    int32_t value;
} T32IocCtrl;

#define OI_T32_TUNING_DIR_SET 0
#define OI_T32_TUNING_DIR_GET 1
typedef struct {                /* DEFAULT_CMD_ISP_TUNING on /dev/isp-m0 */
    int32_t vinum;
    int32_t dir;                /* OI_T32_TUNING_DIR_* */
    uint32_t id;                /* e.g. 0x08000070 sensor fps */
    uint32_t value;             /* value or user pointer, per id */
} T32IocTuning;

typedef struct {                /* framechan REQBUFS */
    uint32_t count;
    uint32_t type;              /* 1 */
    uint32_t memory;            /* 2 = user pointer */
    uint32_t reserved[2];
} T32IocReqBufs;

typedef struct {                /* framechan QUERYBUF/QBUF/DQBUF, 96 bytes */
    uint32_t index;             /* w0 */
    uint32_t type;              /* w1 */
    uint32_t bytesused;         /* w2 */
    uint32_t flags;             /* w3: 0x2002 queued, 0x2004 done */
    uint32_t field;             /* w4 */
    uint32_t ts_sec, ts_usec;   /* w5-6 */
    uint32_t timecode[4];       /* w7-10 */
    uint32_t sequence;          /* w11 */
    uint32_t memory;            /* w12: 2 = user pointer */
    uint32_t m;                 /* w13: Y paddr */
    uint32_t length;            /* w14 */
    uint32_t fps;               /* w15 */
    uint32_t uv_addr;           /* w16 */
    uint32_t stride;            /* w17 */
    uint32_t state;             /* w18 */
    uint32_t geom;              /* w19: height << 16 | width */
    uint32_t reserved2, reserved2_len; /* w20-21 */
    uint32_t reserved, reserved_len;   /* w22-23: w22 user pointer */
} T32IocBuffer;

typedef struct {                /* soc_vpu channel_node, 64 bytes */
    uint32_t clist, vlist;      /* kernel list pointers */
    uint32_t mdelay;
    uint32_t channel_id;
    int32_t vpu_id;
    uint32_t codecdir;
    uint32_t workphase;
    uint32_t status;
    uint32_t output_len;
    uint32_t dma_addr;          /* command list / jpeg_reg_info pa */
    int32_t thread_id;
    uint32_t cmpx;
    uint32_t n_flag;
    uint32_t ncu_addr;
    uint8_t video_type, vtype, sensor_id, frame_type;
    uint32_t hw_time;
} T32IocChannelNode;

_Static_assert(sizeof(T32IocInitArg) == 8, "T32 initarg");
_Static_assert(sizeof(T32IocBufInfo) == 12, "T32 isp_buf_info");
_Static_assert(sizeof(T32IocDbgRegister) == 64, "T32 dbg_register");
_Static_assert(sizeof(T32IocSensorReg) == 116, "T32 sensor register info");
_Static_assert(sizeof(T32IocDriverVersion) == 64, "T32 driver version buffer");
_Static_assert(sizeof(T32IocEnumInput) == 36, "T32 enum input buffer");
_Static_assert(sizeof(T32IocDefaultBin) == 880, "T32 default bin buffer");
_Static_assert(sizeof(T32IocTizianoPar) == 20, "T32 tiziano par (switch bin attr)");
_Static_assert(sizeof(T32IocCtrl) == 8, "T32 tisp_control");
_Static_assert(sizeof(T32IocTuning) == 16, "T32 tuning default ctrl");
_Static_assert(sizeof(T32IocReqBufs) == 20, "T32 requestbuffers");
_Static_assert(sizeof(T32IocBuffer) == 96, "T32 tisp_buffer");
_Static_assert(sizeof(T32IocChannelNode) == 64, "T32 soc_vpu channel_node");

#if defined(PLATFORM_T32)
#include <imp/imp_common.h>
/* AddSensor hands the public object to REGISTER_SENSOR unchanged (#286) */
_Static_assert(sizeof(IMPSensorInfo) == sizeof(T32IocSensorReg),
               "T32 IMPSensorInfo must be the REGISTER_SENSOR payload");
#endif

/* ---- request numbers ---------------------------------------------------- */

/* /dev/tx-isp, DEVICE base 0 */
#define OI_T32_IOC_DRIVER_VERSION                 0x80045401u  /* size field says int; the buffer is 64 B (soc[32], version[32]) */
#define OI_T32_IOC_ENUMINPUT                      0xc0045402u  /* size field says int; {index, name[32]}, -EINVAL past the last input */
#define OI_T32_IOC_G_INPUT                        0x80085403u
#define OI_T32_IOC_S_INPUT                        0xc0085404u  /* out: word1 = w << 16 | h */
#define OI_T32_IOC_REGISTER_SENSOR                0x80745405u  /* = public IMPSensorInfo, passed unchanged */
#define OI_T32_IOC_RELEASE_SENSOR                 0x80745406u
#define OI_T32_IOC_STREAMON                       0x80085407u
#define OI_T32_IOC_STREAMOFF                      0x80085408u
#define OI_T32_IOC_CREATE_SUBDEV_LINKS            0x80085409u
#define OI_T32_IOC_DESTROY_SUBDEV_LINKS           0x8008540au
#define OI_T32_IOC_LINKS_STREAMON                 0xc008540bu
#define OI_T32_IOC_LINKS_STREAMOFF                0xc008540cu
#define OI_T32_IOC_SET_SENSOR_REGISTER            0xc040540du
#define OI_T32_IOC_GET_SENSOR_REGISTER            0x8040540eu
#define OI_T32_IOC_SET_ISP_MEM_INFO               0x800c540fu  /* {vinum, pa, size} */
#define OI_T32_IOC_GET_ISP_MEM_INFO               0x800c5410u  /* driver writes the size */
#define OI_T32_IOC_SET_WDR_BUF_INFO               0x800c5411u
#define OI_T32_IOC_GET_WDR_BUF_INFO               0x800c5412u
#define OI_T32_IOC_ISP_WDR_ENABLE                 0x80045413u
#define OI_T32_IOC_ISP_WDR_DISABLE                0x80045414u
#define OI_T32_IOC_SET_MULTISENSOR_MODE           0xc0045415u
#define OI_T32_IOC_SET_MULTISENSOR_BUF_INFO       0xc00c5416u
#define OI_T32_IOC_GET_MULTISENSOR_BUF_INFO       0x800c5417u
#define OI_T32_IOC_BYPASS_SENSOR                  0x80085418u
#define OI_T32_IOC_SET_DUALSENSOR_SELECT          0xc0045419u
#define OI_T32_IOC_SET_AE_ALGO_FUNC               0xc004541au
#define OI_T32_IOC_GET_AE_ALGO_HANDLE             0xc004541bu
#define OI_T32_IOC_SET_AE_ALGO_HANDLE             0xc004541cu
#define OI_T32_IOC_SET_AE_ALGO_OPEN               0xc004541du
#define OI_T32_IOC_SET_AE_ALGO_CLOSE              0xc004541eu
#define OI_T32_IOC_SET_AE_ALGO_CTRL               0xc004541fu
#define OI_T32_IOC_GET_AE_ALGO_HANDLE_SEC         0xc0045420u
#define OI_T32_IOC_SET_AE_ALGO_HANDLE_SEC         0xc0045421u
#define OI_T32_IOC_SET_AWB_ALGO_FUNC              0xc0045422u
#define OI_T32_IOC_GET_AWB_ALGO_HANDLE            0xc0045423u
#define OI_T32_IOC_SET_AWB_ALGO_HANDLE            0xc0045424u
#define OI_T32_IOC_SET_AWB_ALGO_OPEN              0xc0045425u
#define OI_T32_IOC_SET_AWB_ALGO_CLOSE             0xc0045426u
#define OI_T32_IOC_SET_AWB_ALGO_CTRL              0xc0045427u
#define OI_T32_IOC_GET_AWB_ALGO_HANDLE_SEC        0xc0045428u
#define OI_T32_IOC_SET_AWB_ALGO_HANDLE_SEC        0xc0045429u
#define OI_T32_IOC_SET_DEFAULT_BIN_PATH           0xc004542au
#define OI_T32_IOC_GET_DEFAULT_BIN_PATH           0xc004542bu
#define OI_T32_IOC_SET_FRAME_DROP                 0xc004542cu
#define OI_T32_IOC_GET_FRAME_DROP                 0xc004542du
#define OI_T32_IOC_START_NIGHT_MODE               0xc004542eu
#define OI_T32_IOC_SET_PM_CONTROL                 0xc0045431u
#define OI_T32_IOC_GET_PM_CONTROL                 0xc0045432u
#define OI_T32_IOC_SET_PM_ALL_DONE                0xc0045433u
#define OI_T32_IOC_GET_RAW_CONTROL                0xc0045434u
#define OI_T32_IOC_GET_NV12_CONTROL               0xc0045435u
#define OI_T32_IOC_SET_AE_START_EV                0xc0045436u
#define OI_T32_IOC_GET_AE_START_EV                0xc0045437u
#define OI_T32_IOC_SET_ISP_IPC_MEM_INFO           0x800c5438u
#define OI_T32_IOC_GET_ISP_IPC_MEM_INFO           0x800c5439u  /* driver writes the size (0 = none) */
#define OI_T32_IOC_MEMORY_MALLOC                  0x800c543au  /* in {.., .., size}; out {kva, pa, size} */
#define OI_T32_IOC_MEMORY_FREE                    0x800c543bu  /* {kva, 0, ..} */
#define OI_T32_IOC_ONESHOT_CONTROL                0xc004543cu
#define OI_T32_IOC_GET_RUNNING_MODE               0xc004543du
#define OI_T32_IOC_ONESHOT_DISABLE_DROPCNT        0xc004543eu

/* /dev/isp-m0, TUNING base 100 */
#define OI_T32_IOC_S_CTRL                         0xc0085465u  /* not issued by libimp 2.2.0 in the reference flows */
#define OI_T32_IOC_G_CTRL                         0xc0085466u
#define OI_T32_IOC_DEFAULT_CMD_ISP_TUNING         0xc0105467u  /* {vinum, dir, id, value/ptr} */

/* /dev/misc-ivdc, IVDC base 110 */
#define OI_T32_IOC_SET_JPEG                       0x2000546fu
#define OI_T32_IOC_SET_H264_H265                  0x20005470u
#define OI_T32_IOC_GET_DIRECT                     0xc0045471u  /* 0 = not in direct mode (frames via memory) */
#define OI_T32_IOC_SET_DIRECT                     0xc0045472u
#define OI_T32_IOC_SET_PADDR                      0xc0045473u
#define OI_T32_IOC_MEM_LINE                       0xc0045474u
#define OI_T32_IOC_MAX_STRIDE                     0xc0045475u
#define OI_T32_IOC_SET_MEM_LINE                   0xc0045476u

/* /dev/framechanN, FS base 130 */
#define OI_T32_IOC_SET_FRAME_FORMAT               0xc0045483u  /* int (libimp passes 1); no format struct on T32 */
#define OI_T32_IOC_GET_FRAME_FORMAT               0xc0045484u
#define OI_T32_IOC_REQBUFS                        0xc0145485u  /* {count, type 1, memory 2 = userptr, 0, 0} */
#define OI_T32_IOC_QUERYBUF                       0xc0605486u
#define OI_T32_IOC_QBUF                           0xc0605487u  /* geometry only travels here (geom word) */
#define OI_T32_IOC_DQBUF                          0xc0605488u
#define OI_T32_IOC_FRAME_STREAMON                 0xc0045489u
#define OI_T32_IOC_FRAME_STREAMOFF                0xc004548au
#define OI_T32_IOC_SET_BANKS                      0xc004548bu
#define OI_T32_IOC_LISTEN_BUF                     0xc004548cu  /* blocks, returns the ready count */
#define OI_T32_IOC_SNAP_RAW                       0xc004548eu
#define OI_T32_IOC_CLOSE_MSCA                     0xc004548fu

/* LDC base 180 (T33/T32Pro line, out of scope; device not verified) */
#define OI_T32_IOC_SET_LDC_INIT                   0xc00454b5u
#define OI_T32_IOC_SET_LDC_CONTROL                0xc00454b6u
#define OI_T32_IOC_GET_LDC_CONTROL                0xc00454b7u
#define OI_T32_IOC_LDC_SET_MODE                   0xc00454b8u
#define OI_T32_IOC_LDC_ONLINE_PROBE               0xc00454b9u
#define OI_T32_IOC_LDC_ONLINE_INIT                0xc00454bau
#define OI_T32_IOC_GET_LDC_PAR                    0xc00454bbu
#define OI_T32_IOC_SET_HLDC_BYPASS_STATE          0xc00454bcu
#define OI_T32_IOC_LDC_ONLINE_REFLESH_LUT         0xc00454bdu

/* /dev/tx-isp, DEVICE1 base 200 */
#define OI_T32_IOC_SET_TIZIANO_PAR                0xc00454c8u  /* size field says int; 20 B T32IocTizianoPar (pa first) */
#define OI_T32_IOC_DEFAULT_BIN                    0xc00454c9u  /* size field says int; the buffer is 880 B */
#define OI_T32_IOC_SET_WDR_ENABLE                 0xc00454cau
#define OI_T32_IOC_GPIO_STATE_SWITCH              0xc00454cbu
#define OI_T32_IOC_GPIO_STATE_SWITCH_TRIGGER      0xc00454ccu
#define OI_T32_IOC_SET_FRAME_DROP_ONCE            0xc00454cdu
#define OI_T32_IOC_GET_FRAME_DROP_ONCE            0xc00454ceu

/* /dev/soc_vpu (kernel 4.4.94 drivers/video/ingenic_vpu, type 'c') */
#define OI_T32_VPU_CHANNEL_REQ                    0xc0406300u  /* kernel soc_vpu (not open-tx-isp) */
#define OI_T32_VPU_CHANNEL_REL                    0xc0406301u
#define OI_T32_VPU_CHANNEL_RUN                    0xc0406302u
#define OI_T32_VPU_CHANNEL_START                  0xc0406303u
#define OI_T32_VPU_CHANNEL_WAIT_COMPLETE          0xc0406304u
#define OI_T32_VPU_CHANNEL_FLUSH_CACHE            0xc0406305u
#define OI_T32_VPU_CHANNEL_BUF_INIT               0xc0406306u
#define OI_T32_VPU_CHANNEL_WOR_VPU_REG            0xc0406307u
#define OI_T32_VPU_CHANNEL_VPU_SUSPEND            0xc0406308u
#define OI_T32_VPU_CHANNEL_VPU_RESUME             0xc0406309u
#define OI_T32_VPU_CHANNEL_PRIVATE_TLB            0xc040630au
#define OI_T32_VPU_CHANNEL_WAIT_BSFULL_COMPLETE   0xc040630bu
#define OI_T32_VPU_CHANNEL_SET_BSFULL_REG         0xc040630cu

/* ---- table: device, request, dir, type, nr, size field, real payload, flags */
enum { OI_T32_DEV_ISP, OI_T32_DEV_M0, OI_T32_DEV_IVDC, OI_T32_DEV_FS, OI_T32_DEV_VPU };
#define OI_T32_DEV_PATHS { "/dev/tx-isp", "/dev/isp-m0", "/dev/misc-ivdc", "/dev/framechan", "/dev/soc_vpu" }
#define OI_T32_F_SEEN  1u       /* issued by vendor libimp 2.2.0 in the QEMU reference */
#define OI_T32_F_DRV   2u       /* served by open-tx-isp driver/t32 */
#define OI_T32_F_LATER 4u       /* family out of scope for now (LDC) */

#define OPENIMP_T32_IOCTLS(X) \
    X(ISP, OI_T32_IOC_DRIVER_VERSION, W, 'T', 1, sizeof(int32_t), sizeof(T32IocDriverVersion), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_ENUMINPUT, RW, 'T', 2, sizeof(int32_t), sizeof(T32IocEnumInput), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_G_INPUT, W, 'T', 3, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_S_INPUT, RW, 'T', 4, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_REGISTER_SENSOR, W, 'T', 5, sizeof(T32IocSensorReg), sizeof(T32IocSensorReg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_RELEASE_SENSOR, W, 'T', 6, sizeof(T32IocSensorReg), sizeof(T32IocSensorReg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_STREAMON, W, 'T', 7, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_STREAMOFF, W, 'T', 8, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_CREATE_SUBDEV_LINKS, W, 'T', 9, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_DESTROY_SUBDEV_LINKS, W, 'T', 10, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_LINKS_STREAMON, RW, 'T', 11, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_LINKS_STREAMOFF, RW, 'T', 12, sizeof(T32IocInitArg), sizeof(T32IocInitArg), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_SET_SENSOR_REGISTER, RW, 'T', 13, sizeof(T32IocDbgRegister), sizeof(T32IocDbgRegister), OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_GET_SENSOR_REGISTER, W, 'T', 14, sizeof(T32IocDbgRegister), sizeof(T32IocDbgRegister), OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_SET_ISP_MEM_INFO, W, 'T', 15, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_GET_ISP_MEM_INFO, W, 'T', 16, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_SET_WDR_BUF_INFO, W, 'T', 17, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), 0) \
    X(ISP, OI_T32_IOC_GET_WDR_BUF_INFO, W, 'T', 18, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), 0) \
    X(ISP, OI_T32_IOC_ISP_WDR_ENABLE, W, 'T', 19, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_ISP_WDR_DISABLE, W, 'T', 20, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_MULTISENSOR_MODE, RW, 'T', 21, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_MULTISENSOR_BUF_INFO, RW, 'T', 22, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), 0) \
    X(ISP, OI_T32_IOC_GET_MULTISENSOR_BUF_INFO, W, 'T', 23, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), 0) \
    X(ISP, OI_T32_IOC_BYPASS_SENSOR, W, 'T', 24, sizeof(T32IocInitArg), sizeof(T32IocInitArg), 0) \
    X(ISP, OI_T32_IOC_SET_DUALSENSOR_SELECT, RW, 'T', 25, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AE_ALGO_FUNC, RW, 'T', 26, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_AE_ALGO_HANDLE, RW, 'T', 27, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AE_ALGO_HANDLE, RW, 'T', 28, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AE_ALGO_OPEN, RW, 'T', 29, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AE_ALGO_CLOSE, RW, 'T', 30, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AE_ALGO_CTRL, RW, 'T', 31, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_AE_ALGO_HANDLE_SEC, RW, 'T', 32, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AE_ALGO_HANDLE_SEC, RW, 'T', 33, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AWB_ALGO_FUNC, RW, 'T', 34, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_AWB_ALGO_HANDLE, RW, 'T', 35, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AWB_ALGO_HANDLE, RW, 'T', 36, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AWB_ALGO_OPEN, RW, 'T', 37, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AWB_ALGO_CLOSE, RW, 'T', 38, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AWB_ALGO_CTRL, RW, 'T', 39, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_AWB_ALGO_HANDLE_SEC, RW, 'T', 40, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AWB_ALGO_HANDLE_SEC, RW, 'T', 41, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_DEFAULT_BIN_PATH, RW, 'T', 42, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_DEFAULT_BIN_PATH, RW, 'T', 43, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_FRAME_DROP, RW, 'T', 44, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_FRAME_DROP, RW, 'T', 45, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_START_NIGHT_MODE, RW, 'T', 46, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_PM_CONTROL, RW, 'T', 49, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_PM_CONTROL, RW, 'T', 50, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_PM_ALL_DONE, RW, 'T', 51, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_RAW_CONTROL, RW, 'T', 52, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_NV12_CONTROL, RW, 'T', 53, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_AE_START_EV, RW, 'T', 54, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_AE_START_EV, RW, 'T', 55, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_ISP_IPC_MEM_INFO, W, 'T', 56, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_GET_ISP_IPC_MEM_INFO, W, 'T', 57, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_MEMORY_MALLOC, W, 'T', 58, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_MEMORY_FREE, W, 'T', 59, sizeof(T32IocBufInfo), sizeof(T32IocBufInfo), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_ONESHOT_CONTROL, RW, 'T', 60, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_RUNNING_MODE, RW, 'T', 61, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_ONESHOT_DISABLE_DROPCNT, RW, 'T', 62, sizeof(int32_t), sizeof(int32_t), 0) \
    X(M0, OI_T32_IOC_S_CTRL, RW, 'T', 101, sizeof(T32IocCtrl), sizeof(T32IocCtrl), 0) \
    X(M0, OI_T32_IOC_G_CTRL, RW, 'T', 102, sizeof(T32IocCtrl), sizeof(T32IocCtrl), 0) \
    X(M0, OI_T32_IOC_DEFAULT_CMD_ISP_TUNING, RW, 'T', 103, sizeof(T32IocTuning), sizeof(T32IocTuning), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(IVDC, OI_T32_IOC_SET_JPEG, NONE, 'T', 111, 0, 0, 0) \
    X(IVDC, OI_T32_IOC_SET_H264_H265, NONE, 'T', 112, 0, 0, 0) \
    X(IVDC, OI_T32_IOC_GET_DIRECT, RW, 'T', 113, sizeof(int32_t), sizeof(int32_t), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(IVDC, OI_T32_IOC_SET_DIRECT, RW, 'T', 114, sizeof(int32_t), sizeof(int32_t), OI_T32_F_DRV) \
    X(IVDC, OI_T32_IOC_SET_PADDR, RW, 'T', 115, sizeof(int32_t), sizeof(int32_t), 0) \
    X(IVDC, OI_T32_IOC_MEM_LINE, RW, 'T', 116, sizeof(int32_t), sizeof(int32_t), 0) \
    X(IVDC, OI_T32_IOC_MAX_STRIDE, RW, 'T', 117, sizeof(int32_t), sizeof(int32_t), 0) \
    X(IVDC, OI_T32_IOC_SET_MEM_LINE, RW, 'T', 118, sizeof(int32_t), sizeof(int32_t), 0) \
    X(FS, OI_T32_IOC_SET_FRAME_FORMAT, RW, 'T', 131, sizeof(int32_t), sizeof(int32_t), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_GET_FRAME_FORMAT, RW, 'T', 132, sizeof(int32_t), sizeof(int32_t), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_REQBUFS, RW, 'T', 133, sizeof(T32IocReqBufs), sizeof(T32IocReqBufs), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_QUERYBUF, RW, 'T', 134, sizeof(T32IocBuffer), sizeof(T32IocBuffer), OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_QBUF, RW, 'T', 135, sizeof(T32IocBuffer), sizeof(T32IocBuffer), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_DQBUF, RW, 'T', 136, sizeof(T32IocBuffer), sizeof(T32IocBuffer), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_FRAME_STREAMON, RW, 'T', 137, sizeof(int32_t), sizeof(int32_t), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_FRAME_STREAMOFF, RW, 'T', 138, sizeof(int32_t), sizeof(int32_t), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_SET_BANKS, RW, 'T', 139, sizeof(int32_t), sizeof(int32_t), OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_LISTEN_BUF, RW, 'T', 140, sizeof(int32_t), sizeof(int32_t), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(FS, OI_T32_IOC_SNAP_RAW, RW, 'T', 142, sizeof(int32_t), sizeof(int32_t), 0) \
    X(FS, OI_T32_IOC_CLOSE_MSCA, RW, 'T', 143, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_LDC_INIT, RW, 'T', 181, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_SET_LDC_CONTROL, RW, 'T', 182, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_GET_LDC_CONTROL, RW, 'T', 183, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_LDC_SET_MODE, RW, 'T', 184, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_LDC_ONLINE_PROBE, RW, 'T', 185, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_LDC_ONLINE_INIT, RW, 'T', 186, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_GET_LDC_PAR, RW, 'T', 187, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_SET_HLDC_BYPASS_STATE, RW, 'T', 188, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_LDC_ONLINE_REFLESH_LUT, RW, 'T', 189, sizeof(int32_t), sizeof(int32_t), OI_T32_F_LATER) \
    X(ISP, OI_T32_IOC_SET_TIZIANO_PAR, RW, 'T', 200, sizeof(int32_t), sizeof(T32IocTizianoPar), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_DEFAULT_BIN, RW, 'T', 201, sizeof(int32_t), sizeof(T32IocDefaultBin), OI_T32_F_SEEN|OI_T32_F_DRV) \
    X(ISP, OI_T32_IOC_SET_WDR_ENABLE, RW, 'T', 202, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GPIO_STATE_SWITCH, RW, 'T', 203, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GPIO_STATE_SWITCH_TRIGGER, RW, 'T', 204, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_SET_FRAME_DROP_ONCE, RW, 'T', 205, sizeof(int32_t), sizeof(int32_t), 0) \
    X(ISP, OI_T32_IOC_GET_FRAME_DROP_ONCE, RW, 'T', 206, sizeof(int32_t), sizeof(int32_t), 0) \
    X(VPU, OI_T32_VPU_CHANNEL_REQ, RW, 'c', 0, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), OI_T32_F_SEEN) \
    X(VPU, OI_T32_VPU_CHANNEL_REL, RW, 'c', 1, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), OI_T32_F_SEEN) \
    X(VPU, OI_T32_VPU_CHANNEL_RUN, RW, 'c', 2, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), OI_T32_F_SEEN) \
    X(VPU, OI_T32_VPU_CHANNEL_START, RW, 'c', 3, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), OI_T32_F_SEEN) \
    X(VPU, OI_T32_VPU_CHANNEL_WAIT_COMPLETE, RW, 'c', 4, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), 0) \
    X(VPU, OI_T32_VPU_CHANNEL_FLUSH_CACHE, RW, 'c', 5, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), OI_T32_F_SEEN) \
    X(VPU, OI_T32_VPU_CHANNEL_BUF_INIT, RW, 'c', 6, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), 0) \
    X(VPU, OI_T32_VPU_CHANNEL_WOR_VPU_REG, RW, 'c', 7, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), OI_T32_F_SEEN) \
    X(VPU, OI_T32_VPU_CHANNEL_VPU_SUSPEND, RW, 'c', 8, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), 0) \
    X(VPU, OI_T32_VPU_CHANNEL_VPU_RESUME, RW, 'c', 9, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), 0) \
    X(VPU, OI_T32_VPU_CHANNEL_PRIVATE_TLB, RW, 'c', 10, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), 0) \
    X(VPU, OI_T32_VPU_CHANNEL_WAIT_BSFULL_COMPLETE, RW, 'c', 11, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), OI_T32_F_SEEN) \
    X(VPU, OI_T32_VPU_CHANNEL_SET_BSFULL_REG, RW, 'c', 12, sizeof(T32IocChannelNode), sizeof(T32IocChannelNode), 0)

#define OI_T32_ASSERT_ROW(dev, req, dir, type, nr, size, payload, flags) \
    _Static_assert((req) == OI_T32_IOC(OI_T32_DIR_##dir, type, nr, size), #req " number/size"); \
    _Static_assert((payload) >= (size), #req " payload");
OPENIMP_T32_IOCTLS(OI_T32_ASSERT_ROW)
#undef OI_T32_ASSERT_ROW

#endif /* OPENIMP_T32_IOCTL_H */
