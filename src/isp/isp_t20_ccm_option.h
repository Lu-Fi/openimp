/*
 * OpenIMP option (#102, beyond vendor, opt-in): fitted colour matrices for the
 * T10/T20 Apical ISP.  Included once from isp_tseries.c (needs the table
 * request of the table-ratio helper above it).
 *
 * The Apical 3.12.0 firmware keeps three CCMs per sensor in its calibration
 * tables (API ids 40/41/42 = MT_ABSOLUTE_LS_A / D40 / D50 _CCM_LINEAR, firmware
 * indices 0xd5/0xdc/0xdb, loaded from /etc/sensor/<sensor>.bin).  The AWB state
 * machine picks one of them from the colour temperature (CT < 3426 K -> A,
 * < 4501 K -> D40, else D50; 35-step blend on a change) and the saturation
 * matrix is applied on top every frame (color_matrix_recalculate).  This option
 * only replaces the three table contents through the stock table control
 * 0x8000161 (write: {id, rows 1, cols 9, width 2, 0, buf}); the firmware
 * refreshes its CCMs itself (color_matrix_change_CCMs) and keeps its own
 * light-source selection, blending and saturation.  The day/night switch does
 * not reload these tables, so one write lasts until the sensor parameters are
 * loaded again (module reload / ISP re-init).
 *
 * Enable: OPENIMP_CCM_FILE=<path> in the streamer's environment.  File: one line
 * per light source, "A|D40|D50 m00 m01 m02 m10 m11 m12 m20 m21 m22" (floats,
 * 1.0 = unity, row-major RR RG RB / GR GG GB / BR BG BB); '#' starts a comment;
 * a missing line keeps the vendor table of that light source.  Unset (the
 * default) changes nothing.  Restore the vendor tables: unset the variable and
 * reload the ISP module (or write the vendor values from the bin as a file).
 */
#if defined(PLATFORM_T20)
#define TSERIES_T20_CCM_N 3

static const struct {
    const char *name;
    uint32_t table;     /* apical_api_calibration id */
} tseries_t20_ccm_ids[TSERIES_T20_CCM_N] = {
    { "A", 40 }, { "D40", 41 }, { "D50", 42 },
};

static uint16_t tseries_t20_ccm_val[TSERIES_T20_CCM_N][9];
static uint32_t tseries_t20_ccm_mask;      /* parsed light sources */
static uint32_t tseries_t20_ccm_done;      /* written since EnableTuning */
static int tseries_t20_ccm_state;          /* 0 not read, 1 enabled, -1 off/invalid */

/* float -> Apical sign-magnitude Q8 (bit 15 = sign, 256 = 1.0) */
static uint16_t tseries_t20_ccm_q8(float v)
{
    float a = v < 0.0f ? -v : v;
    uint32_t q = (uint32_t)(a * 256.0f + 0.5f);

    if (q > 0x7ff)
        q = 0x7ff;
    return (uint16_t)(q | (v < 0.0f && q != 0 ? 0x8000u : 0u));
}

static int tseries_t20_ccm_load(void)
{
    const char *path = getenv("OPENIMP_CCM_FILE");
    char line[256];
    FILE *f;

    if (tseries_t20_ccm_state != 0)
        return tseries_t20_ccm_state;
    tseries_t20_ccm_state = -1;
    if (path == NULL || path[0] == '\0')
        return -1;
    f = fopen(path, "r");
    if (f == NULL) {
        IMP_LOG_ERR("IMP-ISP", "OPENIMP_CCM_FILE %s: %s, vendor CCMs kept", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[8];
        float m[9];
        char *hash = strchr(line, '#');
        int i, k;

        if (hash != NULL)
            *hash = '\0';
        if (sscanf(line, "%7s %f %f %f %f %f %f %f %f %f", name, &m[0], &m[1], &m[2],
                   &m[3], &m[4], &m[5], &m[6], &m[7], &m[8]) != 10)
            continue;
        for (k = 0; k < TSERIES_T20_CCM_N; k++)
            if (strcmp(name, tseries_t20_ccm_ids[k].name) == 0)
                break;
        if (k == TSERIES_T20_CCM_N)
            continue;
        for (i = 0; i < 9; i++) {
            if (!(m[i] > -7.99f && m[i] < 7.99f))
                break;
            tseries_t20_ccm_val[k][i] = tseries_t20_ccm_q8(m[i]);
        }
        if (i == 9)
            tseries_t20_ccm_mask |= 1u << k;
    }
    fclose(f);
    if (tseries_t20_ccm_mask == 0) {
        IMP_LOG_ERR("IMP-ISP", "OPENIMP_CCM_FILE %s: no valid A/D40/D50 line, vendor CCMs kept", path);
        return -1;
    }
    tseries_t20_ccm_state = 1;
    return 1;
}

/* Write the parsed tables that are not written yet; the firmware refuses the
 * write (-EINVAL) while the ISP context is not initialised, then the next
 * caller (SetISPRunningMode) tries again. */
static void tseries_t20_ccm_apply(const char *why)
{
    int k;

    if (tseries_t20_ccm_load() != 1)
        return;
    for (k = 0; k < TSERIES_T20_CCM_N; k++) {
        TSeriesTableReq req;
        uint16_t buf[9];

        if (!(tseries_t20_ccm_mask & (1u << k)) || (tseries_t20_ccm_done & (1u << k)))
            continue;
        memcpy(buf, tseries_t20_ccm_val[k], sizeof(buf));
        memset(&req, 0, sizeof(req));
        req.table = tseries_t20_ccm_ids[k].table;
        req.rows = 1;
        req.cols = 9;
        req.elem_size = 2;
        req.flag = 0;
        req.buf = buf;
        if (tseries_tuning_set_ptr(TISP_CID_TABLE_TUNING, &req) == 0) {
            tseries_t20_ccm_done |= 1u << k;
            IMP_LOG_WARN("IMP-ISP", "OPENIMP_CCM_FILE: %s CCM written (%s)",
                         tseries_t20_ccm_ids[k].name, why);
        } else {
            IMP_LOG_WARN("IMP-ISP", "OPENIMP_CCM_FILE: %s CCM not written yet (%s, errno %d)",
                         tseries_t20_ccm_ids[k].name, why, errno);
        }
    }
}
#endif /* PLATFORM_T20 */
