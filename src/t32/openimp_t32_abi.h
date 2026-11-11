/*
 * OpenIMP T32 platform skeleton (#286): ABI facts that the public headers
 * do not carry yet.
 *
 * First target: T32 PRJ007 with the vendor 2.x kernel ABI (the open-tx-isp
 * driver/t32 ABI; ioctl bases DEVICE 0, TUNING 100, IVDC 110, FS 130).
 * libimp 1.0.6 uses another ioctl table and is not a target.
 *
 * Public struct sizes (mipsel o32) are asserted where the types live:
 *   IMPSensorInfo 116            include/imp/imp_common.h
 *   IMPFrameInfo 48              include/imp/imp_common.h   (T41 layout)
 *   IMPFSChnAttr 104             include/imp/imp_framesource.h (T41 layout)
 *   IMPEncoderCHNAttr 184, IMPEncoderRcAttr 132, IMPEncoderAttrRcMode 64,
 *   IMPEncoderStream 16, IMPEncoderPack 32
 *                                include/imp/imp_encoder.h  (T23 family API)
 *   IMPAudioIOAttr 24, IMPAudioFrame 32, IMPAudioIChnParam 12
 *                                openimp_t32_skeleton.c (T41 layout)
 * The OSD region attribute is laid out below; the public imp_osd.h type is
 * the generic 72-byte one on every SoC (the OSD module keeps its vendor
 * layouts in src/t23/openimp_t23_osd_abi.h), so the T32 layout is pinned
 * here until the T32 OSD issue moves it there.
 */
#ifndef OPENIMP_T32_ABI_H
#define OPENIMP_T32_ABI_H

#include <stddef.h>
#include <stdint.h>

/*
 * IMPOSDRgnAttr, T32 2.x: 272 bytes.  Same prefix as T41 1.2.5 (type,
 * rect, line, fmt, data), but the ISP draw block is 80 bytes (T41: 76),
 * so fontData (104) and mosaicAttr (28) move by 4, and a 2-bit OSD
 * attribute (mask, bit0 format, bit1 format) closes the object.
 */
typedef struct {
    int type;                       /* 0x000 IMPOsdRgnType */
    int rect[4];                    /* 0x004 IMPRect */
    int line[2];                    /* 0x014 IMPLine */
    int fmt;                        /* 0x01c IMPPixelFormat */
    uint32_t data[4];               /* 0x020 IMPOSDRgnAttrData */
    uint32_t ispdraw[20];           /* 0x030 IMPOSDIspDraw (80) */
    uint32_t fontData[26];          /* 0x080 IMPOSDFontAttrData (104) */
    int mosaicAttr[7];              /* 0x0e8 IMPOSDMosaicAttr (28) */
    uint32_t twobit_mask;           /* 0x104 IMPOSD2BitAttr */
    uint32_t twobit_bit0_fmt;
    uint32_t twobit_bit1_fmt;
} T32OSDRgnAttrABI;

_Static_assert(offsetof(T32OSDRgnAttrABI, fmt) == 28, "T32 OSD fmt");
_Static_assert(offsetof(T32OSDRgnAttrABI, data) == 32, "T32 OSD data");
_Static_assert(offsetof(T32OSDRgnAttrABI, ispdraw) == 48, "T32 OSD osdispdraw");
_Static_assert(offsetof(T32OSDRgnAttrABI, fontData) == 128, "T32 OSD fontData");
_Static_assert(offsetof(T32OSDRgnAttrABI, mosaicAttr) == 232, "T32 OSD mosaicAttr");
_Static_assert(offsetof(T32OSDRgnAttrABI, twobit_mask) == 260, "T32 OSD bitAttr");
_Static_assert(sizeof(T32OSDRgnAttrABI) == 272, "T32 IMPOSDRgnAttr ABI mismatch (vendor 2.x: 272 bytes)");

/* IVS interface object (13 words, as on T41) */
typedef struct {
    void *param;
    int paramSize;
    int pixfmt;
    void *fn[9];                    /* init .. flushFrame */
    void *priv;
} T32IVSInterfaceABI;

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(T32IVSInterfaceABI) == 52, "T32 IMPIVSInterface ABI mismatch");
#endif

#endif /* OPENIMP_T32_ABI_H */
