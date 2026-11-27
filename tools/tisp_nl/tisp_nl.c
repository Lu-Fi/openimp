/*
 * tisp_nl (#102): client for the T21 tisp tuning netlink channel (protocol 23,
 * replies to port 50), the channel the vendor tuning tool uses.  Both the
 * vendor tx-isp-t21.ko and open-tx-isp tx_isp_t21 serve it from
 * tisp_param_operate_process (open-tx-isp driver/t21/tx_isp_t21_recovered.c,
 * netlink_rcv_msg / tisp_param_operate_process / tisp_ccm_param_array_set).
 * The socket exists while the ISP is initialised (tisp_init .. tisp_deinit).
 *
 * Message = 6 x int32 header {type, id, len, a3, a4, a5} + payload.
 *   type 0 get array <id>      reply header + array bytes
 *   type 1 set array <id>      header len = payload bytes; reply header only
 *   type 3 id 5 get WB attr    7 words {mode, manual r, manual b, statis r, statis b, global r, global b}
 *   type 2 id 5 set WB attr    payload 7 words; driver calls tisp_s_wb_mode(mode, r, b) (1 = manual, 0 = auto; gains Q8)
 * CCM ids: 0x7c dp_cfg, 0x7d A / 0x7e T / 0x7f D linear CCM (9 x int32, 14-bit two's complement Q10,
 * row-major RR RG RB / GR GG GB / BR BG BB), 0x80 EV list, 0x81 saturation list (9, 256 = 1.0), 0x82 AWB CT list (2).
 *
 * usage: tisp_nl get ID            -> "ID w0 w1 ..." (decimal int32)
 *        tisp_nl set ID w0 w1 ...  -> "ok"
 *        tisp_nl wbget             -> "wb mode r b statis_r statis_b global_r global_b"
 *        tisp_nl wbset MODE R B    -> "ok"
 * Exit 0 = answer received, 1 = usage, 2 = socket/bind (port 50 busy: another tuning client), 3 = no answer within 2 s.
 * One client at a time (fixed port 50).  Opt-in test tool: changes stay until the ISP re-reads the bin (module reload,
 * day/night parameter switch); write the saved arrays back to restore.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <linux/netlink.h>

#define TISP_NL_PROTO 23
#define TISP_NL_PORT 50
#define HDR_WORDS 6
#define MAX_WORDS 64

static int nl_open(void)
{
    struct sockaddr_nl sa;
    struct timeval tv = { 2, 0 };
    int fd = socket(AF_NETLINK, SOCK_RAW, TISP_NL_PROTO);

    if (fd < 0) {
        fprintf(stderr, "tisp_nl: socket(proto %d): %s\n", TISP_NL_PROTO, strerror(errno));
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = TISP_NL_PORT;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "tisp_nl: bind(port %d): %s\n", TISP_NL_PORT, strerror(errno));
        close(fd);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* send header + payload; receive the reply into out (words after the header); returns payload words or -1 */
static int nl_xfer(int fd, const int32_t hdr[HDR_WORDS], const int32_t *data, int nwords, int32_t *out, int outmax)
{
    union {
        struct nlmsghdr h;
        char b[NLMSG_SPACE((HDR_WORDS + MAX_WORDS) * 4)];
    } m;
    struct sockaddr_nl to;
    int len = (HDR_WORDS + nwords) * 4, n;

    memset(&m, 0, sizeof(m));
    m.h.nlmsg_len = NLMSG_LENGTH(len);
    m.h.nlmsg_pid = TISP_NL_PORT;
    memcpy(NLMSG_DATA(&m.h), hdr, HDR_WORDS * 4);
    if (nwords)
        memcpy((char *)NLMSG_DATA(&m.h) + HDR_WORDS * 4, data, nwords * 4);
    memset(&to, 0, sizeof(to));
    to.nl_family = AF_NETLINK;
    if (sendto(fd, &m, m.h.nlmsg_len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        fprintf(stderr, "tisp_nl: sendto: %s\n", strerror(errno));
        return -1;
    }
    memset(&m, 0, sizeof(m));
    n = recv(fd, &m, sizeof(m), 0);
    if (n < (int)NLMSG_LENGTH(HDR_WORDS * 4)) {
        fprintf(stderr, "tisp_nl: no answer (%s)\n", n < 0 ? strerror(errno) : "short");
        return -2;
    }
    n = ((int)m.h.nlmsg_len - (int)NLMSG_LENGTH(HDR_WORDS * 4)) / 4;
    if (n > outmax)
        n = outmax;
    if (n > 0 && out)
        memcpy(out, (char *)NLMSG_DATA(&m.h) + HDR_WORDS * 4, n * 4);
    return n < 0 ? 0 : n;
}

int main(int argc, char **argv)
{
    int32_t hdr[HDR_WORDS] = { 0 }, data[MAX_WORDS], out[MAX_WORDS];
    int fd, n, i, nw = 0;

    if (argc < 2)
        goto usage;
    if (!strcmp(argv[1], "get") && argc == 3) {
        hdr[0] = 0; hdr[1] = (int32_t)strtol(argv[2], NULL, 0);
    } else if (!strcmp(argv[1], "set") && argc >= 4 && argc - 3 <= MAX_WORDS) {
        hdr[0] = 1; hdr[1] = (int32_t)strtol(argv[2], NULL, 0);
        for (i = 3; i < argc; i++)
            data[nw++] = (int32_t)strtol(argv[i], NULL, 0);
        hdr[2] = nw * 4;
    } else if (!strcmp(argv[1], "wbget") && argc == 2) {
        hdr[0] = 3; hdr[1] = 5;
    } else if (!strcmp(argv[1], "wbset") && argc == 5) {
        hdr[0] = 2; hdr[1] = 5;
        memset(data, 0, 7 * 4);
        for (i = 0; i < 3; i++)
            data[i] = (int32_t)strtol(argv[2 + i], NULL, 0);
        nw = 7;
        hdr[2] = nw * 4;
    } else {
        goto usage;
    }
    fd = nl_open();
    if (fd < 0)
        return 2;
    n = nl_xfer(fd, hdr, data, nw, out, MAX_WORDS);
    close(fd);
    if (n < 0)
        return n == -1 ? 2 : 3;
    if (hdr[0] == 0) {
        printf("%s", argv[2]);
        for (i = 0; i < n; i++)
            printf(" %d", out[i]);
        printf("\n");
    } else if (hdr[0] == 3) {
        printf("wb");
        for (i = 0; i < n && i < 7; i++)
            printf(" %d", out[i]);
        printf("\n");
    } else {
        printf("ok\n");
    }
    return 0;
usage:
    fprintf(stderr, "usage: tisp_nl get ID | set ID w0 .. | wbget | wbset MODE R B\n");
    return 1;
}
