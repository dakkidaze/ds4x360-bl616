/*
 * R06/R07 EP0 boundary tests over usbfs (Linux).
 *
 * Usage:
 *   ep0_boundary_test /dev/bus/usb/<bus>/<dev>
 *   ep0_boundary_test --self-test        (no device, checks the verdict logic)
 *
 * Verdict rules (from the R06/R07 review):
 *   - A negative case passes ONLY on a real protocol STALL, i.e. ioctl
 *     returning -1 with EPIPE. ESHUTDOWN / ECOMM / ETIMEDOUT are transport
 *     failures and are NOT evidence that the firmware stalled.
 *   - EINVAL means the Linux host rejected the request before it reached the
 *     device (usbfs check_ctrlrecip and friends). Such a case is reported
 *     SKIP: neither the firmware nor the host proved anything.
 *   - A positive case must transfer exactly the expected number of bytes.
 *
 * Summary/exit semantics (from the 3rd-round review):
 *   - no FAIL, no SKIP      -> "ALL PASS", exit 0
 *   - any FAIL              -> "FAILED", exit 1
 *   - no FAIL but some SKIP -> "INCOMPLETE (cases unverified)", exit 3
 *   A skipped case must never be folded into a success result: "some cases
 *   were never verified" is not "all cases passed".
 *
 * Exit codes: 0 all cases passed, 1 at least one failure, 2 usage/open error,
 *             3 no failure but unverified (skipped) cases remain.
 *
 * Build: gcc -O2 -o tools/ep0_boundary_test tools/ep0_boundary_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#define VERDICT_PASS 0
#define VERDICT_FAIL 1
#define VERDICT_SKIP 2

static int ctrl(int fd, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                uint16_t len, void *data)
{
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof(c));
    c.bRequestType = rt;
    c.bRequest = req;
    c.wValue = val;
    c.wIndex = idx;
    c.wLength = len;
    c.timeout = 2000;
    c.data = data;
    return ioctl(fd, USBDEVFS_CONTROL, &c);
}

/* Pure verdict logic, split out so --self-test can exercise it without a
 * device. expect_len < 0 disables the length check. */
static int judge(int r, int err, int expect_stall, int expect_len)
{
    if (expect_stall) {
        if (r >= 0)
            return VERDICT_FAIL;          /* request was accepted: wrong */
        if (err == EPIPE)
            return VERDICT_PASS;          /* real STALL */
        if (err == EINVAL)
            return VERDICT_SKIP;          /* host rejected before the device */
        return VERDICT_FAIL;              /* transport error: no evidence */
    }

    if (r < 0)
        return VERDICT_FAIL;
    if (expect_len >= 0 && r != expect_len)
        return VERDICT_FAIL;
    return VERDICT_PASS;
}

/* ---- result accounting -------------------------------------------------- */

#define SUMMARY_ALL_PASS   0
#define SUMMARY_FAILED     1
#define SUMMARY_INCOMPLETE 3

static int g_pass;
static int g_fail;
static int g_skip;

static void tally(int v)
{
    if (v == VERDICT_PASS)
        g_pass++;
    else if (v == VERDICT_SKIP)
        g_skip++;
    else
        g_fail++;
}

/* The final state is also the process exit code (0 / 1 / 3), so a caller
 * cannot mistake "some cases were skipped" for success. */
static int summary_state(int pass, int fail, int skip)
{
    (void)pass;
    if (fail > 0)
        return SUMMARY_FAILED;
    if (skip > 0)
        return SUMMARY_INCOMPLETE;
    return SUMMARY_ALL_PASS;
}

static const char *summary_word(int state)
{
    switch (state) {
    case SUMMARY_ALL_PASS:
        return "ALL PASS";
    case SUMMARY_FAILED:
        return "FAILED";
    default:
        return "INCOMPLETE (cases unverified)";
    }
}

static int run(int fd, const char *name, uint8_t rt, uint8_t req, uint16_t val,
               uint16_t idx, uint16_t len, void *data, int expect_stall,
               int expect_len, int expect_byte)
{
    errno = 0;
    int r = ctrl(fd, rt, req, val, idx, len, data);
    int err = r < 0 ? errno : 0;
    int v = judge(r, err, expect_stall, expect_len);

    if (v == VERDICT_PASS && !expect_stall && expect_byte >= 0 &&
        (r < 1 || ((uint8_t *)data)[0] != (uint8_t)expect_byte))
        v = VERDICT_FAIL;

    const char *word = v == VERDICT_PASS ? "PASS"
                     : v == VERDICT_SKIP ? "SKIP" : "FAIL";
    printf("%-46s ret=%2d errno=%2d(%-11s) expect=%-5s -> %s",
           name, r, err, r < 0 ? strerror(err) : "ok",
           expect_stall ? "STALL" : "OK", word);
    if (v == VERDICT_SKIP)
        printf("  (host rejected before device)");
    if (v == VERDICT_FAIL && !expect_stall && r >= 0 && expect_len >= 0 &&
        r != expect_len)
        printf("  (short/long transfer: %d != %d)", r, expect_len);
    printf("\n");

    return v;
}

static int self_test(void)
{
    static const struct {
        int r, err, expect_stall, expect_len, want;
        const char *name;
    } cases[] = {
        { -1, EPIPE,     1, -1, VERDICT_PASS, "STALL is EPIPE" },
        { -1, ESHUTDOWN, 1, -1, VERDICT_FAIL, "ESHUTDOWN is not a STALL" },
        { -1, ECOMM,     1, -1, VERDICT_FAIL, "ECOMM is not a STALL" },
        { -1, ETIMEDOUT, 1, -1, VERDICT_FAIL, "timeout is not a STALL" },
        { -1, EINVAL,    1, -1, VERDICT_SKIP, "host EINVAL is SKIP" },
        {  0, 0,         1, -1, VERDICT_FAIL, "accepted negative fails" },
        {  2, 0,         0,  2, VERDICT_PASS, "positive exact length" },
        {  2, 0,         0,  1, VERDICT_FAIL, "positive wrong length" },
        {  1, 0,         0,  1, VERDICT_PASS, "positive 1 byte" },
        { -1, EPIPE,     0, -1, VERDICT_FAIL, "positive stalled" },
    };

    int fails = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = judge(cases[i].r, cases[i].err, cases[i].expect_stall,
                        cases[i].expect_len);
        int ok = (got == cases[i].want);
        if (!ok)
            fails++;
        printf("%-34s want=%d got=%d -> %s\n", cases[i].name, cases[i].want,
               got, ok ? "PASS" : "FAIL");
    }

    /* Summary accounting: a skipped case must never turn into ALL PASS, and
     * the exit code must carry the same distinction. Replaying the counts
     * through tally() also checks the counters themselves. */
    static const struct {
        int pass, fail, skip, want_state;
        const char *want_word;
    } sums[] = {
        { 10, 0, 0, SUMMARY_ALL_PASS,   "ALL PASS" },
        {  9, 0, 1, SUMMARY_INCOMPLETE, "INCOMPLETE (cases unverified)" },
        {  0, 0, 6, SUMMARY_INCOMPLETE, "INCOMPLETE (cases unverified)" },
        {  5, 1, 2, SUMMARY_FAILED,     "FAILED" },
        {  0, 1, 0, SUMMARY_FAILED,     "FAILED" },
    };

    for (unsigned i = 0; i < sizeof(sums) / sizeof(sums[0]); i++) {
        g_pass = g_fail = g_skip = 0;
        for (int k = 0; k < sums[i].pass; k++)
            tally(VERDICT_PASS);
        for (int k = 0; k < sums[i].fail; k++)
            tally(VERDICT_FAIL);
        for (int k = 0; k < sums[i].skip; k++)
            tally(VERDICT_SKIP);

        int state = summary_state(g_pass, g_fail, g_skip);
        int ok = (g_pass == sums[i].pass) && (g_fail == sums[i].fail) &&
                 (g_skip == sums[i].skip) && (state == sums[i].want_state) &&
                 (strcmp(summary_word(state), sums[i].want_word) == 0);
        if (!ok)
            fails++;
        printf("summary p=%2d f=%d s=%d -> %-31s state=%d exit=%d %s\n",
               g_pass, g_fail, g_skip, summary_word(state), state, state,
               ok ? "PASS" : "FAIL");
    }

    printf("---- self-test %s (%d failures) ----\n",
           fails ? "FAILED" : "ALL PASS", fails);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <usbdev> | --self-test\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--self-test") == 0)
        return self_test();

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    g_pass = g_fail = g_skip = 0;
    uint8_t buf[16];

    /* UAC1 class requests to the audio control interface (0).
     * Speaker Feature Unit id 2: volume CS 0x02, mute CS 0x01, channel 0.
     * Sampling-frequency control is the endpoint control (EP 0x01, CS 0x01). */
    const uint8_t RT_SET = 0x21;              /* host->device, class, interface */
    const uint16_t vol_val  = (0x02 << 8);    /* VOLUME, channel 0 */
    const uint16_t mute_val = (0x01 << 8);    /* MUTE, channel 0 */
    const uint16_t fu_idx   = (0x02 << 8);    /* FU2 << 8 | interface 0 */
    const uint8_t RT_EP_SET = 0x22;           /* host->device, class, endpoint */
    const uint16_t freq_val = (0x01 << 8);    /* EP sampling freq, CS 0x01 */
    const uint16_t ep_out   = 0x01;           /* DS4_AUDIO_EP_OUT (normal build) */

    /* ---- R06: volume is exactly 2 bytes; 8/1/0 must STALL ---- */
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R06 volume SET_CUR wLength=8 (bad)", RT_SET, 0x01,
                 vol_val, fu_idx, 8, buf, 1, -1, -1) );
    tally(run(fd, "R06 volume SET_CUR wLength=1 (bad)", RT_SET, 0x01,
                 vol_val, fu_idx, 1, buf, 1, -1, -1) );
    tally(run(fd, "R06 volume SET_CUR wLength=0 (bad)", RT_SET, 0x01,
                 vol_val, fu_idx, 0, buf, 1, -1, -1) );

    buf[0] = 0x00; buf[1] = 0xFF;             /* -1 dB, 2-byte value */
    tally(run(fd, "R06 volume SET_CUR wLength=2 (good)", RT_SET, 0x01,
                 vol_val, fu_idx, 2, buf, 0, 2, -1) );

    /* ---- R06: mute is exactly 1 byte; 4/2 must STALL ---- */
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R06 mute   SET_CUR wLength=4 (bad)", RT_SET, 0x01,
                 mute_val, fu_idx, 4, buf, 1, -1, -1) );
    tally(run(fd, "R06 mute   SET_CUR wLength=2 (bad)", RT_SET, 0x01,
                 mute_val, fu_idx, 2, buf, 1, -1, -1) );
    buf[0] = 0x00;
    tally(run(fd, "R06 mute   SET_CUR wLength=1 (good)", RT_SET, 0x01,
                 mute_val, fu_idx, 1, buf, 0, 1, -1) );

    /* ---- R06: UAC1 sampling frequency is exactly 3 bytes ---- */
    uint8_t freq[4] = { 0x80, 0xBB, 0x00, 0x00 };   /* 48000 Hz, 3 B */
    tally(run(fd, "R06 freq   SET_CUR wLength=2 (bad)", RT_EP_SET, 0x01,
                 freq_val, ep_out, 2, freq, 1, -1, -1) );
    tally(run(fd, "R06 freq   SET_CUR wLength=4 (bad)", RT_EP_SET, 0x01,
                 freq_val, ep_out, 4, freq, 1, -1, -1) );

    /* ---- R07: standard interface requests ---- */
    const uint8_t RT_STD_SET = 0x01;
    const uint8_t RT_STD_GET = 0x81;

    /* Real interface + real alt: accepted. */
    tally(run(fd, "R07 SET_INTERFACE if0 alt0 (good)", RT_STD_SET, 0x0B,
                 0, 0, 0, NULL, 0, 0, -1) );
    tally(run(fd, "R07 SET_INTERFACE if3 alt0 (good)", RT_STD_SET, 0x0B,
                 0, 3, 0, NULL, 0, 0, -1) );
    /* Undescribed alt, non-existent interface, out-of-range index. */
    tally(run(fd, "R07 SET_INTERFACE if3 alt5 (bad)", RT_STD_SET, 0x0B,
                 5, 3, 0, NULL, 1, -1, -1) );
    tally(run(fd, "R07 SET_INTERFACE if15 alt0 (bad)", RT_STD_SET, 0x0B,
                 0, 15, 0, NULL, 1, -1, -1) );
    tally(run(fd, "R07 SET_INTERFACE wIndex=200 (bad)", RT_STD_SET, 0x0B,
                 0, 200, 0, NULL, 1, -1, -1) );
    /* Reserved high bytes must be rejected, not aliased onto a valid target. */
    tally(run(fd, "R07 SET_INTERFACE wValue=0x0100 (bad)", RT_STD_SET, 0x0B,
                 0x0100, 0, 0, NULL, 1, -1, -1) );
    tally(run(fd, "R07 SET_INTERFACE wIndex=0x0100 (bad)", RT_STD_SET, 0x0B,
                 0, 0x0100, 0, NULL, 1, -1, -1) );
    /* SET_INTERFACE has no data stage. */
    uint8_t dummy = 0;
    tally(run(fd, "R07 SET_INTERFACE wLength=1 (bad)", RT_STD_SET, 0x0B,
                 0, 0, 1, &dummy, 1, -1, -1) );
    /* Wrong direction for a SET. */
    tally(run(fd, "R07 SET_INTERFACE wrong dir (bad)", RT_STD_GET, 0x0B,
                 0, 0, 0, NULL, 1, -1, -1) );

    memset(buf, 0xFF, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE if0 (good)", RT_STD_GET, 0x0A,
                 0, 0, 1, buf, 0, 1, 0) );
    memset(buf, 0xFF, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE if3 (good)", RT_STD_GET, 0x0A,
                 0, 3, 1, buf, 0, 1, 0) );
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE if15 (bad)", RT_STD_GET, 0x0A,
                 0, 15, 1, buf, 1, -1, -1) );
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE wIndex=200 (bad)", RT_STD_GET, 0x0A,
                 0, 200, 1, buf, 1, -1, -1) );
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE wIndex=0x0100 (bad)", RT_STD_GET, 0x0A,
                 0, 0x0100, 1, buf, 1, -1, -1) );
    /* GET_INTERFACE returns exactly one byte. */
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE wLength=2 (bad)", RT_STD_GET, 0x0A,
                 0, 0, 2, buf, 1, -1, -1) );
    /* Wrong direction for a GET. */
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE wrong dir (bad)", RT_STD_SET, 0x0A,
                 0, 0, 1, buf, 1, -1, -1) );
    /* wValue is reserved for GET_INTERFACE. */
    memset(buf, 0, sizeof(buf));
    tally(run(fd, "R07 GET_INTERFACE wValue!=0 (bad)", RT_STD_GET, 0x0A,
                 1, 0, 1, buf, 1, -1, -1) );

    close(fd);

    int state = summary_state(g_pass, g_fail, g_skip);
    printf("---- %s (pass=%d fail=%d skip=%d) ----\n",
           summary_word(state), g_pass, g_fail, g_skip);
    return state;
}
