/**
 * IMP Common Definitions
 * 
 * Common types, enums, and structures used across IMP modules
 */

#ifndef __IMP_COMMON_H__
#define __IMP_COMMON_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/**
 * Return codes
 */
#define IMP_SUCCESS 0
#define IMP_FAILURE -1

/**
 * Blocking mode
 */
typedef enum {
    BLOCK = 0,      /**< Blocking mode */
    NOBLOCK = 1     /**< Non-blocking mode */
} IMPBlock;

/**
 * Device IDs for binding
 */
#define DEV_ID_FS       0   /**< Frame Source */
#define DEV_ID_ENC      1   /**< Encoder */
#define DEV_ID_IVS      3   /**< IVS */
#define DEV_ID_OSD      4   /**< OSD */

/**
 * Video input interface (T40/T41)
 */
typedef enum {
    IMPVI_MAIN = 0,     /**< Main video input */
    IMPVI_SEC = 1       /**< Secondary video input */
} IMPVI;

/**
 * Pixel format
 *
 * Ingenic vendor numbering: identical in the T20 3.12.0 / T21 1.0.33 /
 * T23 1.3.0 / T30 1.0.5 / T31 1.1.x / T40 / T41 headers.  These values are
 * ABI -- the vendor IMP_FrameSource_SetChnAttr compares attr->pixFmt against
 * PIX_FMT_RAW (34) in every libimp build and pixfmt_to_string() bounds the
 * value by PIX_FMT_NB (36).  The older table that OpenIMP used to carry
 * (PIX_FMT_YUVJ* at 7..9, BGRA/RGBA at 12/13, Bayer at 14..17, RAW at 18)
 * comes from an unrelated ffmpeg-derived imp_common.h in the buildroot
 * sysroot and matches no Ingenic SDK: consumers using it pass BGRA where
 * libimp expects RGB24, and 18 (= BGGR8 here) where libimp expects RAW.
 */
typedef enum {
    PIX_FMT_YUV420P = 0,      /**< Planar YUV 4:2:0, 12 bpp */
    PIX_FMT_YUYV422 = 1,      /**< Packed YUV 4:2:2, Y0 Cb Y1 Cr */
    PIX_FMT_UYVY422 = 2,      /**< Packed YUV 4:2:2, Cb Y0 Cr Y1 */
    PIX_FMT_YUV422P = 3,      /**< Planar YUV 4:2:2, 16 bpp */
    PIX_FMT_YUV444P = 4,      /**< Planar YUV 4:4:4, 24 bpp */
    PIX_FMT_YUV410P = 5,      /**< Planar YUV 4:1:0, 9 bpp */
    PIX_FMT_YUV411P = 6,      /**< Planar YUV 4:1:1, 12 bpp */
    PIX_FMT_GRAY8 = 7,        /**< 8 bpp gray */
    PIX_FMT_MONOWHITE = 8,    /**< 1 bpp, 0 is white */
    PIX_FMT_MONOBLACK = 9,    /**< 1 bpp, 0 is black */
    PIX_FMT_NV12 = 10,        /**< Semi-planar YUV 4:2:0, U then V */
    PIX_FMT_NV21 = 11,        /**< Semi-planar YUV 4:2:0, V then U */
    PIX_FMT_RGB24 = 12,       /**< Packed RGB 8:8:8, 24 bpp */
    PIX_FMT_BGR24 = 13,       /**< Packed BGR 8:8:8, 24 bpp */
    PIX_FMT_ARGB = 14,        /**< Packed ARGB 8:8:8:8, 32 bpp */
    PIX_FMT_RGBA = 15,        /**< Packed RGBA 8:8:8:8, 32 bpp */
    PIX_FMT_ABGR = 16,        /**< Packed ABGR 8:8:8:8, 32 bpp */
    PIX_FMT_BGRA = 17,        /**< Packed BGRA 8:8:8:8, 32 bpp */
    PIX_FMT_RGB565BE = 18,    /**< Packed RGB 5:6:5, 16 bpp, big-endian */
    PIX_FMT_RGB565LE = 19,    /**< Packed RGB 5:6:5, 16 bpp, little-endian */
    PIX_FMT_RGB555BE = 20,    /**< Packed RGB 5:5:5, 16 bpp, big-endian */
    PIX_FMT_RGB555LE = 21,    /**< Packed RGB 5:5:5, 16 bpp, little-endian */
    PIX_FMT_BGR565BE = 22,    /**< Packed BGR 5:6:5, 16 bpp, big-endian */
    PIX_FMT_BGR565LE = 23,    /**< Packed BGR 5:6:5, 16 bpp, little-endian */
    PIX_FMT_BGR555BE = 24,    /**< Packed BGR 5:5:5, 16 bpp, big-endian */
    PIX_FMT_BGR555LE = 25,    /**< Packed BGR 5:5:5, 16 bpp, little-endian */
    PIX_FMT_0RGB = 26,        /**< Packed RGB 8:8:8 in the low 24 bits */
    PIX_FMT_RGB0 = 27,        /**< Packed RGB 8:8:8 in the high 24 bits */
    PIX_FMT_0BGR = 28,        /**< Packed BGR 8:8:8 in the low 24 bits */
    PIX_FMT_BGR0 = 29,        /**< Packed BGR 8:8:8 in the high 24 bits */
    PIX_FMT_BAYER_BGGR8 = 30, /**< Bayer BGGR, 8-bit samples */
    PIX_FMT_BAYER_RGGB8 = 31, /**< Bayer RGGB, 8-bit samples */
    PIX_FMT_BAYER_GBRG8 = 32, /**< Bayer GBRG, 8-bit samples */
    PIX_FMT_BAYER_GRBG8 = 33, /**< Bayer GRBG, 8-bit samples */
    PIX_FMT_RAW = 34,         /**< Raw data (ISP bypass / raw sensor) */
    PIX_FMT_HSV = 35,         /**< HSV */
    PIX_FMT_NB = 36,          /**< Number of pixel formats */
} IMPPixelFormat;

_Static_assert(PIX_FMT_NV12 == 10, "PIX_FMT_NV12 ABI mismatch");
_Static_assert(PIX_FMT_NV21 == 11, "PIX_FMT_NV21 ABI mismatch");
_Static_assert(PIX_FMT_RAW == 34, "PIX_FMT_RAW ABI mismatch");
_Static_assert(PIX_FMT_NB == 36, "PIX_FMT_NB ABI mismatch");

/**
 * Cell structure for binding modules
 */
typedef struct {
    int deviceID;       /**< Device ID */
    int groupID;        /**< Group ID */
    int outputID;       /**< Output ID */
} IMPCell;

/**
 * Version information
 */
typedef struct {
    char aVersion[64];  /**< Version string */
} IMPVersion;

/** Legacy T23 frame descriptor with direct-mode timestamps. */
#if defined(PLATFORM_T23)
typedef struct {
    int index;
    int pool_idx;
    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;
    uint32_t size;
    uint32_t phyAddr;
    uint32_t virAddr;
    uint32_t direct_phyAddr;
    int64_t timeStamp;
    int64_t timeStamp_ivdc;
    uint32_t priv[0];
} IMPFrameInfo;

typedef struct {
    uint64_t ts;
    uint64_t minus;
    uint64_t plus;
} IMPFrameTimestamp;

_Static_assert(offsetof(IMPFrameInfo, direct_phyAddr) == 0x20,
               "legacy IMPFrameInfo.direct_phyAddr ABI mismatch");
_Static_assert(offsetof(IMPFrameInfo, timeStamp) == 0x28,
               "legacy IMPFrameInfo.timeStamp ABI mismatch");
_Static_assert(sizeof(IMPFrameInfo) == 0x38,
               "legacy IMPFrameInfo ABI mismatch");
#elif defined(PLATFORM_T21) || defined(PLATFORM_T30)
typedef struct {
    int index;
    int pool_idx;
    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;
    uint32_t size;
    uint32_t phyAddr;
    uint32_t virAddr;
    int64_t timeStamp;
    uint32_t priv[0];
} IMPFrameInfo;

typedef struct {
    uint64_t ts;
    uint64_t minus;
    uint64_t plus;
} IMPFrameTimestamp;

_Static_assert(offsetof(IMPFrameInfo, timeStamp) == 0x20,
               "legacy IMPFrameInfo.timeStamp ABI mismatch");
_Static_assert(sizeof(IMPFrameInfo) == 0x28,
               "legacy IMPFrameInfo ABI mismatch");
#else
typedef struct {
    int width;
    int height;
} IMPFrameInfo;
#endif

/** T23 encoder payload identifiers (SDK 1.3.0). */
typedef enum {
    PT_JPEG = 0,
    PT_H264 = 1,
    PT_H265 = 2,
} IMPPayloadType;

/**
 * Rectangle structure
 */
typedef struct {
    int x;              /**< X coordinate */
    int y;              /**< Y coordinate */
    int width;          /**< Width */
    int height;         /**< Height */
} IMPRect;

/**
 * Point structure
 */
typedef struct {
    int x;              /**< X coordinate */
    int y;              /**< Y coordinate */
} IMPPoint;

/**
 * Sensor control interface type
 */
typedef enum {
    TX_SENSOR_CONTROL_INTERFACE_I2C = 1,    /**< I2C interface */
    TX_SENSOR_CONTROL_INTERFACE_SPI = 2     /**< SPI interface */
} TXSensorControlBusType;

/**
 * I2C configuration
 * NOTE: Platform-specific layout! T23 has different field order than T31.
 */
#if defined(PLATFORM_T23)
typedef struct {
    char type[20];      /**< Sensor type string */
    int addr;           /**< I2C address */
} TXSNSI2CConfig;
#else
typedef struct {
    char type[20];      /**< Sensor type string */
    int addr;           /**< I2C address */
    int i2c_adapter;    /**< I2C adapter number */
} TXSNSI2CConfig;
#endif

/**
 * Sensor information (platform-specific layout)
 *
 * Structure size varies by platform to match kernel driver expectations:
 * - T31/T21/C100: 80 bytes (0x50) - no private_data field
 * - T23: 84 bytes (0x54) - cbus_type at offset 0x24 (36), i2c_adapter at offset 0x40 (64)
 * - T40/T41: Extended structure with additional fields
 *
 * T23 kernel reads:
 *   *(arg3 + 0x24) = cbus_type (offset 36)
 *   *(arg3 + 0x40) = i2c_adapter (offset 64)
 */
#if defined(PLATFORM_T23)
typedef struct {
    char name[32];                          /**< Sensor name (0-31) */
    int reserved1;                          /**< Reserved/padding (32-35) */
    TXSensorControlBusType cbus_type;       /**< Control bus type at offset 0x24 (36-39) */
    TXSNSI2CConfig i2c;                     /**< I2C: type[20] + addr[4] = 24 bytes (40-63) */
    int i2c_adapter;                        /**< I2C adapter at offset 0x40 (64-67) */
    int rst_gpio;                           /**< Reset GPIO (68-71) */
    int pwdn_gpio;                          /**< Power down GPIO (72-75) */
    int power_gpio;                         /**< Power GPIO (76-79) */
    int sensor_id;                          /**< Sensor ID (80-83) */
    /* Total: 32+4+4+24+4+4+4+4+4 = 84 bytes */
} IMPSensorInfo;
#else
typedef struct {
    char name[32];                          /**< Sensor name */
    TXSensorControlBusType cbus_type;       /**< Control bus type */
    TXSNSI2CConfig i2c;                     /**< I2C configuration */
    int rst_gpio;                           /**< Reset GPIO */
    int pwdn_gpio;                          /**< Power down GPIO */
    int power_gpio;                         /**< Power GPIO */
    int sensor_id;                          /**< Sensor ID */
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    void *private_data;                     /**< Private data (T40/T41) */
#endif
    /* Note: T31/T21/C100 have no field here, keeping struct at 80 bytes */
} IMPSensorInfo;
#endif

/**
 * Region handle type
 */
typedef int IMPRgnHandle;

#ifdef __cplusplus
}
#endif

#endif /* __IMP_COMMON_H__ */
