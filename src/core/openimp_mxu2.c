/*
 * MXU2 runtime selection: SIGILL-guarded selftest, see openimp_mxu2.h.
 */
#include "core/openimp_mxu2.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !OPENIMP_MXU2_ASM
struct openimp_vreg openimp_vr[32];
int openimp_mxu2_model_fault;
#endif

static pthread_mutex_t mxu2_lock = PTHREAD_MUTEX_INITIALIZER;
static sigjmp_buf mxu2_jmp;
static volatile sig_atomic_t mxu2_in_probe;
static pthread_t mxu2_probe_thread;

static void mxu2_sigill(int sig)
{
    (void)sig;
    if (mxu2_in_probe && pthread_equal(pthread_self(), mxu2_probe_thread))
        siglongjmp(mxu2_jmp, 1);
    /* not ours: restore the default action; returning re-executes the
     * faulting instruction, which then terminates as before */
    signal(SIGILL, SIG_DFL);
}

int openimp_mxu_env_allowed(void)
{
    const char *e = getenv("OPENIMP_MXU");

    return !(e && (strcmp(e, "0") == 0 || strcmp(e, "off") == 0));
}

int openimp_mxu2_select(const char *name, int (*selftest)(void))
{
    struct sigaction sa, old;
    volatile int ok = 0;
    const char *why;

#if defined(PLATFORM_T23) || (defined(PLATFORM_T21) && !defined(PLATFORM_T20) && !defined(PLATFORM_T10))
    /* The T23 core has no MXU/COP2 and its kernel does not raise SIGILL for a
     * COP2 instruction: the CU2 handler restores an MXU context and oopses in
     * xburst_cop2_restore, the watchdog then resets the board (T23N .30,
     * 2026-11-05). The T21 kernel does the same: the default MXU2 selection
     * reset the T21N board three times when a rotated channel started
     * (PC420 .24, 2026-11-08), while OPENIMP_MXU=0 rotates cleanly. The
     * SIGILL-guarded selftest below must never run on these SoCs, so the build
     * never probes: always the C path (Opus review finding B1). */
    (void)selftest;
    fprintf(stderr, "[MXU] %s: C (no usable MXU2 on this SoC)\n", name);
    return 0;
#endif
    if (!openimp_mxu_env_allowed()) {
        fprintf(stderr, "[MXU] %s: C (OPENIMP_MXU=0)\n", name);
        return 0;
    }
    pthread_mutex_lock(&mxu2_lock);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mxu2_sigill;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, &old) != 0) {
        pthread_mutex_unlock(&mxu2_lock);
        fprintf(stderr, "[MXU] %s: C (no SIGILL guard)\n", name);
        return 0;
    }
    mxu2_probe_thread = pthread_self();
    if (sigsetjmp(mxu2_jmp, 1) == 0) {
        mxu2_in_probe = 1;
        ok = selftest() ? 1 : 0;
        why = ok ? "MXU2 (selftest bit-exact)" : "C (MXU2 selftest mismatch)";
    } else {
        ok = 0;
        why = "C (no MXU2 for this process: SIGILL)";
    }
    mxu2_in_probe = 0;
    sigaction(SIGILL, &old, NULL);
    pthread_mutex_unlock(&mxu2_lock);
    fprintf(stderr, "[MXU] %s: %s\n", name, why);
    return ok;
}
