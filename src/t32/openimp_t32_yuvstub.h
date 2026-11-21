/* T32/T33 unbound YUV / JPEG encoder, stage 1 (#752): the vendor 2.0.2.1 argument checks and return codes
 * as pure functions (no encoder back end).  Vendor: YuvInit 0x1040c4, YuvEncode 0x104b4c, YuvExit 0x105248,
 * InputJpege 0x105430, SetChnQpgAI_Yuv 0x1048ac.  Used by openimp_t32_enc_e3.c, host test tests/t33/yuv_stub_test.c. */
#ifndef OPENIMP_T32_YUVSTUB_H
#define OPENIMP_T32_YUVSTUB_H

#define YUVSTUB_E_PARAM  ((int)0x80040002)  /* codec not 1/2, bad alignment, foreign handle */
#define YUVSTUB_E_NULL   ((int)0x80040010)  /* NULL handle */
#define YUVSTUB_E_DEV    ((int)0x80048000)  /* i264e_init / i265e_init / ijpege_init failed (vendor code) */
#define YUVSTUB_PT_H264  1
#define YUVSTUB_PT_H265  2

/* YuvInit: codec first, then width % 16 / height even (both 0x80040002); a valid request would reach the
 * encoder init, which the stub cannot do: 0x80048000.  NULL encIn: vendor crashes, here 0x80040002. */
static inline int yuvstub_init_rc(int have_in, int type, int w, int h)
{
    if (!have_in || (type != YUVSTUB_PT_H264 && type != YUVSTUB_PT_H265))
        return YUVSTUB_E_PARAM;
    if ((w & 0xf) != 0 || (h & 1) != 0)
        return YUVSTUB_E_PARAM;
    return YUVSTUB_E_DEV;
}

/* YuvEncode / YuvExit: NULL 0x80040010; any other handle was not made by the stub: 0x80040002 */
static inline int yuvstub_handle_rc(int have_handle)
{
    return have_handle ? YUVSTUB_E_PARAM : YUVSTUB_E_NULL;
}

/* InputJpege: width % 16 or height odd 0x80040002 (vendor line 0x242e), else the init fails: 0x80048000 */
static inline int yuvstub_jpege_rc(int w, int h)
{
    return ((w & 0xf) != 0 || (h & 1) != 0) ? YUVSTUB_E_PARAM : YUVSTUB_E_DEV;
}

#endif
