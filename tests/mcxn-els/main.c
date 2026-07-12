/*
 * MCXN947 ELS (EdgeLock Secure subsystem) honesty test.
 *
 * The ELS is a crypto engine we do not implement.  The ONLY safe behaviour is to
 * tell the guest so.  A crypto engine that reports success without computing is
 * the most dangerous silent-wrong a machine model can contain: every real ELS
 * command writes its result by DMA into a guest buffer (ELS_DMA_RES0), so
 * "succeeding" without touching that buffer hands firmware UNINITIALISED MEMORY
 * as its signature, digest, session key or ciphertext — and an ECDSA verify would
 * then "succeed" against garbage.
 *
 * This test asserts the guest is TOLD:
 *
 *   1. A cryptographic command (HASH, ECSIGN, CIPHER) must FAIL through the
 *      engine's documented error channel — ELS_STATUS[ELS_ERR] and
 *      ELS_ERR_STATUS[OPN_ERR] — and must NOT write the result buffer.
 *   2. The failure must be reported, not hung: ELS_BUSY must still clear, so a
 *      driver's "wait for done" loop terminates.  (Faulting an NXP accelerator
 *      through its completion path hangs the driver instead of informing it.)
 *   3. The error must be clearable (ELS_ERR_STATUS_CLR) so firmware can recover.
 *   4. RND_REQ — the one command we CAN honestly satisfy — must actually deliver
 *      random bytes into the result buffer, not zeros.
 *
 * Prints "ELS PASS" only if every check holds.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t*)(LP+0x14))
#define LP_CTRL (*(volatile uint32_t*)(LP+0x18))
#define LP_DATA (*(volatile uint32_t*)(LP+0x1C))
static void putc_(char c){ while(!(LP_STAT&(1u<<23))){} LP_DATA=(uint8_t)c; }
static void puts_(const char*s){ while(*s) putc_(*s++); }

#define ELS 0x40054000u
#define ELS_STATUS         (*(volatile uint32_t*)(ELS+0x00))
#define ELS_CTRL           (*(volatile uint32_t*)(ELS+0x04))
#define ELS_DMA_RES0       (*(volatile uint32_t*)(ELS+0x38))
#define ELS_DMA_RES0_LEN   (*(volatile uint32_t*)(ELS+0x3C))
#define ELS_ERR_STATUS     (*(volatile uint32_t*)(ELS+0x4C))
#define ELS_ERR_STATUS_CLR (*(volatile uint32_t*)(ELS+0x50))

/* ELS_STATUS (CMSIS S50_ELS_STATUS_*) */
#define ST_BUSY   (1u << 0)
#define ST_ERR    (1u << 2)
/* ELS_ERR_STATUS */
#define ERR_OPN   (1u << 1)
/* ELS_CTRL */
#define CTRL_EN    (1u << 0)
#define CTRL_START (1u << 1)
#define CTRL_CMD(c) ((uint32_t)(c) << 3)

/* Command IDs (MCUXpresso mcuxClEls_Crc.h). */
#define CMD_CIPHER   0
#define CMD_ECSIGN   4
#define CMD_HASH     20
#define CMD_RND_REQ  24

/* A result buffer in SRAM, well clear of the stack. */
#define RESBUF 0x20009000u
#define RESLEN 32

static void els_run(uint32_t cmd)
{
    ELS_CTRL = CTRL_EN | CTRL_START | CTRL_CMD(cmd);
    /* Must terminate: the engine reports failure, it does not hang. */
    while (ELS_STATUS & ST_BUSY) {
    }
}

void cpu0_main(void)
{
    volatile uint32_t *res = (volatile uint32_t *)RESBUF;
    static const int crypto_cmds[3] = { CMD_HASH, CMD_ECSIGN, CMD_CIPHER };
    int ok = 1;

    LP_CTRL = (1u << 19);
    puts_("ELS test\r\n");

    ELS_DMA_RES0 = RESBUF;
    ELS_DMA_RES0_LEN = RESLEN;

    /* Clean slate. */
    ELS_ERR_STATUS_CLR = 0xFFFFFFFFu;
    ok &= !(ELS_STATUS & ST_ERR);
    ok &= (ELS_ERR_STATUS == 0);

    /* --- 1/2/3: every real crypto command must FAIL, visibly, and recover --- */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < RESLEN / 4; j++) {
            res[j] = 0xEEEEEEEEu;         /* poison: must NOT be overwritten */
        }

        els_run(crypto_cmds[i]);          /* terminates => not hung */

        ok &= !!(ELS_STATUS & ST_ERR);    /* the guest is TOLD */
        ok &= !!(ELS_ERR_STATUS & ERR_OPN);

        /* The result buffer must be untouched: we did not fabricate a digest,
         * a signature or a ciphertext. */
        for (int j = 0; j < RESLEN / 4; j++) {
            ok &= (res[j] == 0xEEEEEEEEu);
        }

        ELS_ERR_STATUS_CLR = 0xFFFFFFFFu; /* firmware can clear and retry */
        ok &= !(ELS_STATUS & ST_ERR);
        ok &= (ELS_ERR_STATUS == 0);
    }

    /* --- 4: RND_REQ is the one command we can honestly satisfy ------------- */
    for (int j = 0; j < RESLEN / 4; j++) {
        res[j] = 0;
    }
    els_run(CMD_RND_REQ);

    ok &= !(ELS_STATUS & ST_ERR);         /* no error: we really did this one */
    {
        int nonzero = 0, distinct = 0;

        for (int j = 0; j < RESLEN / 4; j++) {
            if (res[j] != 0) {
                nonzero++;
            }
            if (j && res[j] != res[j - 1]) {
                distinct++;
            }
        }
        /* Real entropy: not a buffer of zeros, and not one value repeated. */
        ok &= (nonzero >= (RESLEN / 4) - 1);
        ok &= (distinct >= (RESLEN / 4) - 2);
    }

    puts_(ok ? "ELS PASS\r\n" : "ELS FAIL\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
