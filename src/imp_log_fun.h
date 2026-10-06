/*
 * imp_log_fun.h -- the one declaration of the OEM-shaped logging entry
 * points exported by libimp.so (implemented in core/imp_log.c).
 *
 * The IMP_LOG_OP_* / IMP_LOG_OUT_* values below are the OEM ABI definitions
 * from Ingenic's imp/imp_log.h (identical in T31 1.1.5/1.1.6 and A1 1.7.0).
 * They live in this private header rather than in an installed header so a
 * translation unit that also includes the vendor (or open-stack) imp_log.h
 * cannot see two different definitions.
 */
#ifndef IMP_LOG_FUN_H
#define IMP_LOG_FUN_H

/* OEM printing options: bit N selects field N of the rendered line. */
#ifndef IMP_LOG_OP_PID
#define IMP_LOG_OP_PID     0x01     /* process id        (not rendered) */
#define IMP_LOG_OP_USTIME  0x02     /* microsecond time  (not rendered) */
#define IMP_LOG_OP_MODULE  0x04     /* module/tag        (always on here) */
#define IMP_LOG_OP_FILE    0x08     /* source file                       */
#define IMP_LOG_OP_FUNC    0x10     /* function name                     */
#define IMP_LOG_OP_LINE    0x20     /* source line                       */
#define IMP_LOG_OP_NONE    0x00
#define IMP_LOG_OP_ALL     0x3f
#define IMP_LOG_OP_DEFAULT IMP_LOG_OP_ALL
#endif

/* OEM output sinks: the third argument of imp_log_fun(). */
#ifndef IMP_LOG_OUT_STDOUT
#define IMP_LOG_OUT_STDOUT     0
#define IMP_LOG_OUT_LOCAL_FILE 1
#define IMP_LOG_OUT_SERVER     2    /* logcat on Android builds */
#define IMP_LOG_OUT_DEFAULT    IMP_LOG_OUT_STDOUT
#endif

int IMP_Log_Get_Option(void);
void IMP_Log_Set_Option(int option);
int imp_log_fun(int level, int option, int type, ...);

#endif /* IMP_LOG_FUN_H */
