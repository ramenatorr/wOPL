// retrobox: wOPL asks the retrobox brain what to launch (retro-gaming-box-poc
// docs/superpowers/specs/2026-09-30-ps2-opl-http-launch-design.md). With no valid retrobox.cfg on the share this
// file does nothing and wOPL behaves like stock.
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <kernel.h>
#include <delaythread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ps2ips.h>
#include <libcdvd-common.h>

#include "include/common.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/menusys.h"
#include "include/module.h"
#include "include/pad.h"
#include "include/supportbase.h"
#include "include/ethsupport.h"
#include "include/retrobox_parse.h"
#include "include/retrobox_launch.h"

_Static_assert(RB_MEDIA_CD == SCECdPS2CD, "retrobox media CD");
_Static_assert(RB_MEDIA_DVD == SCECdPS2DVD, "retrobox media DVD");

#define RB_CONNECT_MS 1000

// libcglue hands F_SETFL on a socket to ps2ips' __ps2ipcFcntlfsetflHelper, which passes only (flags & 1) on as
// FIONBIO -- lwIP's own O_NONBLOCK value. newlib's O_NONBLOCK (0x4000) has bit 0 clear and left the socket blocking
// (a blocking connect waited ~18 s on the 2026-09-30 console trace).
#define RB_LWIP_O_NONBLOCK 1

#define RB_TOTAL_MS 2000

#define RB_POLL_STEP_US 10000 // 10 ms between socket polls


static int rbEnabled;
static struct sockaddr_in rbAddr;
static char rbHost[24];            // "a.b.c.d:port" for the Host header
static volatile int rbPollPending; // a poll is queued or running on the IO thread
static volatile int rbJobReady;    // rbJob holds a job for the GUI thread
static rb_job_t rbJob;
static int rbPollGames;
static int rbPolledOnce;
static clock_t rbLastPollEnd; // when the last poll finished
static int rbPollDelay;       // ms to the next poll: rbPollDelayMs(last exchange ok)
static clock_t rbJobAt;       // when rbJob arrived

// Diagnostics: with "trace = 1" in retrobox.cfg, a step trace (first ~3 KB) is rewritten to <share>retrobox.log.
static char rbTraceBuf[3072];
static int rbTraceLen;
static char rbTracePath[64];
static int rbTraceOn;

static void rbTraceFlush(void)
{
    int fd;

    if (rbTraceOn && (fd = open(rbTracePath, O_WRONLY | O_CREAT | O_TRUNC, 0666)) >= 0) {
        write(fd, rbTraceBuf, rbTraceLen);
        close(fd);
    }
}

static void rbTraceV(const char *fmt, va_list ap)
{
    int n;

    if (!rbTracePath[0] || rbTraceLen > (int)sizeof(rbTraceBuf) - 200)
        return;
    n = snprintf(rbTraceBuf + rbTraceLen, sizeof(rbTraceBuf) - rbTraceLen, "%lu ", (unsigned long)(clock() / 1000));
    rbTraceLen += n;
    n = vsnprintf(rbTraceBuf + rbTraceLen, sizeof(rbTraceBuf) - rbTraceLen - 1, fmt, ap);
    rbTraceLen += n;
    rbTraceBuf[rbTraceLen++] = '\n';
    rbTraceFlush();
}

static void rbTrace(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    rbTraceV(fmt, ap);
    va_end(ap);
}

void retroboxTrace(const char *fmt, ...)
{
    va_list ap;

    if (!rbTraceOn)
        return; // other devices scan before the SMB share is up, or with no "trace = 1": nothing to record
    va_start(ap, fmt);
    rbTraceV(fmt, ap);
    va_end(ap);
}

static char rbStepBuf[7168];
static int rbStepLen;
static char rbStepPath[64];
static volatile int rbStepArmed;
static int rbStepSema = -1;
volatile int retroboxRenderTrace;

void retroboxStep(const char *fmt, ...)
{
    va_list ap;
    int n, fd;

    if (!rbStepArmed || rbStepSema < 0)
        return;
    WaitSema(rbStepSema);
    if (rbStepLen <= (int)sizeof(rbStepBuf) - 240) {
        n = snprintf(rbStepBuf + rbStepLen, sizeof(rbStepBuf) - rbStepLen, "%lu t%d ", (unsigned long)(clock() / 1000), GetThreadId());
        rbStepLen += n;
        va_start(ap, fmt);
        n = vsnprintf(rbStepBuf + rbStepLen, 200, fmt, ap);
        va_end(ap);
        rbStepLen += n > 199 ? 199 : n;
        rbStepBuf[rbStepLen++] = '\n';
        if ((fd = open(rbStepPath, O_WRONLY | O_CREAT | O_TRUNC, 0666)) >= 0) {
            write(fd, rbStepBuf, rbStepLen);
            close(fd);
        }
    }
    SignalSema(rbStepSema);
}

void retroboxStepEnd(void)
{
    rbStepArmed = 0; // the network goes away next
    retroboxRenderTrace = 0;
}

static int rbMsLeft(clock_t start)
{
    unsigned long spent = rbElapsedMs(start, clock(), CLOCKS_PER_SEC);
    return spent >= RB_TOTAL_MS ? 0 : (int)(RB_TOTAL_MS - spent);
}

// One connection for every exchange (HTTP/1.1 keep-alive). The IOP runs out of client sockets after ~4 connections
// and a closed one stays unusable for 30 s+ whoever closes first (console trace 2026-09-30), so a fresh connection
// per poll cannot last. Used by the IO thread (boot report, polls) and the GUI thread (claim/error), never at once:
// the GUI thread only reports once a poll has finished.
static int rbSock = -1;

static void rbDisconnect(void)
{
    if (rbSock >= 0) {
        close(rbSock);
        rbSock = -1;
    }
}

// An idle keep-alive connection the server has closed (uvicorn: after 5 s idle) reads EOF; anything else counts as
// alive (ps2ips reports "nothing to read" and real errors alike as -23; a dead one then fails and is replaced).
static int rbReusable(void)
{
    char c;

    if (rbSock < 0)
        return 0;
    if (recv(rbSock, &c, 1, 0) == 0) {
        rbDisconnect();
        return 0;
    }
    return 1;
}

static int rbConnect(void)
{
    int n;

    rbSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    rbTrace("socket -> %d", rbSock);
    if (rbSock < 0)
        return -1;
    fcntl(rbSock, F_SETFL, RB_LWIP_O_NONBLOCK);
    n = connect(rbSock, (struct sockaddr *)&rbAddr, sizeof(rbAddr));
    rbTrace("connect %s -> %d", rbHost, n);
    return 0;
}

// Send req and read one whole reply on rbSock. No select(): it hung the IO thread on the console (2026-09-30), and
// ps2ips reports every socket error as -23, so each step polls the non-blocking socket every RB_POLL_STEP_US within
// the deadlines. fresh: the connection is new, so a failing send may still be the handshake in progress.
static int rbExchangeOn(const char *req, char *resp, int respLen, int fresh)
{
    clock_t start = clock();
    int n, sent = 0, got = 0, len = strlen(req);

    while (sent < len) {
        n = send(rbSock, req + sent, len - sent, 0);
        if (n > 0) {
            sent += n;
            continue;
        }
        if (!fresh || rbElapsedMs(start, clock(), CLOCKS_PER_SEC) >= RB_CONNECT_MS) {
            rbTrace("send failed (%s connection)", fresh ? "new" : "reused");
            return -1;
        }
        DelayThread(RB_POLL_STEP_US);
    }
    while (got < respLen - 1) {
        n = recv(rbSock, resp + got, respLen - 1 - got, 0);
        if (n > 0) {
            got += n;
            resp[got] = '\0';
            if (rbResponseComplete(resp, got)) {
                rbTrace("exchange ok (%s): sent %d, got %d in %lu ms", fresh ? "new" : "reused", sent, got,
                        rbElapsedMs(start, clock(), CLOCKS_PER_SEC));
                return got;
            }
            continue;
        }
        if (n == 0) {
            rbTrace("closed by the server after %d bytes", got);
            return -1;
        }
        if (rbMsLeft(start) <= 0) {
            rbTrace("recv: %d bytes by the %d ms deadline", got, RB_TOTAL_MS);
            return -1;
        }
        DelayThread(RB_POLL_STEP_US);
    }
    return -1; // reply larger than the buffer
}

// Returns the bytes read, or -1. A reused connection that fails is replaced once.
static int rbExchange(const char *req, char *resp, int respLen)
{
    int reused = rbReusable(), got;

    if (!reused && rbConnect() < 0)
        return -1;
    got = rbExchangeOn(req, resp, respLen, !reused);
    if (got < 0) {
        rbDisconnect();
        if (reused && rbConnect() == 0) {
            got = rbExchangeOn(req, resp, respLen, 1);
            if (got < 0)
                rbDisconnect();
        }
    }
    return got;
}

static void rbPost(const char *path, const char *body)
{
    char req[512], resp[256];

    if (rbFormatRequest(req, sizeof(req), "POST", path, rbHost, body) > 0)
        rbExchange(req, resp, sizeof(resp));
}

static void rbPoll(void)
{
    char path[48], req[256], resp[1024];
    rb_job_t job;
    int ok = 0;

    snprintf(path, sizeof(path), "/opl/next?games=%d", rbPollGames);
    if (rbFormatRequest(req, sizeof(req), "GET", path, rbHost, NULL) > 0 && rbExchange(req, resp, sizeof(resp)) > 0) {
        ok = 1;
        if (rbParseStatus(resp) == 200 && rbParseJob(resp, &job) == 0) {
            rbJob = job;
            rbJobAt = clock();
            rbJobReady = 1;
        }
    }
    // The next poll counts from the end of this one, and backs off while the brain does not answer.
    rbTrace("poll games=%d ok=%d job=%d", rbPollGames, ok, rbJobReady);
    rbPollDelay = rbPollDelayMs(ok);
    rbLastPollEnd = clock();
    rbPolledOnce = 1;
    rbPollPending = 0;
}

void retroboxInit(const char *prefix)
{
    char path[64], text[256];
    rb_addr_t addr;
    int fd, n;

    rbEnabled = 0;
    rbSock = -1; // an SMB re-init: any old connection went with the old network session
    rbTraceOn = 0;
    rbTraceLen = 0;
    snprintf(rbTracePath, sizeof(rbTracePath), "%sretrobox.log", prefix);
    snprintf(rbStepPath, sizeof(rbStepPath), "%sretrobox-launch.log", prefix);
    if (rbStepSema < 0) {
        ee_sema_t sema;
        memset(&sema, 0, sizeof(sema));
        sema.init_count = 1;
        sema.max_count = 1;
        rbStepSema = CreateSema(&sema);
    }
    rbStepArmed = 1;
    rbStepLen = 0;
    retroboxStep("debug build ready");
    rbStepArmed = 0;
    snprintf(path, sizeof(path), "%sretrobox.cfg", prefix);
    fd = open(path, O_RDONLY, 0666);
    rbTrace("init open %s -> %d errno=%d", path, fd, errno);
    if (fd < 0)
        return;
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    rbTrace("read -> %d", n);
    if (n <= 0)
        return;
    text[n] = '\0';
    n = rbParseConfig(text, &addr);
    rbTrace("parse -> %d ip=%s port=%d trace=%d", n, n == 0 ? addr.ip : "-", n == 0 ? addr.port : 0, n == 0 ? addr.trace : 0);
    if (n != 0)
        return;
    rbTraceOn = addr.trace;
    rbTraceFlush(); // the lines buffered so far
    // Byte by byte, in network order: inet_addr() returns 0 here (libcglue's inet ops are only filled by the EE-side
    // lwIP, which wOPL does not link), which sent every SYN to 0.0.0.0 on the 2026-09-30 console trace.
    memset(&rbAddr, 0, sizeof(rbAddr));
    rbAddr.sin_len = sizeof(rbAddr);
    rbAddr.sin_family = AF_INET;
    ((unsigned char *)&rbAddr.sin_port)[0] = (unsigned char)(addr.port >> 8);
    ((unsigned char *)&rbAddr.sin_port)[1] = (unsigned char)(addr.port & 0xFF);
    memcpy(&rbAddr.sin_addr, addr.octet, 4);
    snprintf(rbHost, sizeof(rbHost), "%s:%d", addr.ip, addr.port);
    rbEnabled = 1;
    LOG("RETROBOX brain %s\n", rbHost);
    rbPost("/opl/event", "name=smb_connected\n");
}

// The ItemCounter's "list loaded" test (themes.c drawItemCounter) for the ETH list: its node count, or 0.
static int rbEthLoadedCount(void)
{
    item_list_t *support = list_support[ETH_MODE].support;
    submenu_list_t *node;
    int count, total = 0;

    if (!support || !support->enabled || gNetworkStartup != 0)
        return 0;
    count = support->itemGetCount(support);
    if (count <= 0)
        return 0;
    guiLock(); // the IO thread frees these nodes under this lock on a list refresh (menuClearGameList)
    for (node = list_support[ETH_MODE].menuItem.submenu; node; node = node->next)
        total++;
    guiUnlock();
    return total == count ? count : 0;
}

static void rbActOnJob(void)
{
    opl_io_module_t *mod = &list_support[ETH_MODE];
    item_list_t *support = mod->support;
    submenu_list_t *node, *hit = NULL;
    int hits = 0;
    char body[96];

    rbStepArmed = 1;
    rbStepLen = 0;
    retroboxStep("job %s media=%d name=[%s]", rbJob.id, rbJob.media, rbJob.name);
    guiLock(); // the IO thread clears the list under this lock (menuClearGameList)
    for (node = mod->menuItem.submenu; node; node = node->next) {
        base_game_info_t *game = (base_game_info_t *)support->itemGet(support, node->item.id);
        if (game && game->media == rbJob.media && strcmp(game->name, rbJob.name) == 0) {
            hit = node;
            hits++;
        }
    }
    if (hits == 1) {
        mod->menuItem.current = hit;
        mod->menuItem.pagestart = hit;
    }
    guiUnlock();
    retroboxStep("hits=%d id=%d", hits, hit ? hit->item.id : -1);

    if (hits != 1) {
        snprintf(body, sizeof(body), "job=%s\nreason=%s\n", rbJob.id, hits ? "ambiguous" : "not_found");
        rbPost("/opl/error", body);
        return;
    }
    snprintf(body, sizeof(body), "job=%s\n", rbJob.id);
    rbPost("/opl/claimed", body); // before the launch: it tears the network down
    retroboxStep("claim posted; socket left open");
    // Not closed here: close() hung the GUI thread for some games (2026-11-09: the debug log stopped right here), and
    // the launch tears the whole network down anyway. If the launch does not happen, polling reuses this connection.
    menuSetSelectedItem(&mod->menuItem);
    retroboxStep("selected item set; gSelectButton=%d", gSelectButton);
    retroboxRenderTrace = 1;
    // Exactly as the autostart firing (menuHandleInputMain): per-game config, VMCs and compat modes all apply.
    // Returns only if the launch did not happen (a declined VMC prompt, an error box); polling then resumes.
    if (gSelectButton == KEY_CIRCLE)
        mod->menuItem.execCircle(&mod->menuItem);
    else
        mod->menuItem.execCross(&mod->menuItem);
    retroboxStep("exec returned: no launch happened");
}

void retroboxTick(void)
{
    int count;

    if (!rbEnabled)
        return;
    if (rbJobReady) {
        if (!rbJobFresh(rbElapsedMs(rbJobAt, clock(), CLOCKS_PER_SEC))) {
            rbJobReady = 0; // the brain has given up on it by now: never launch late, never claim
        } else if (rbEthLoadedCount() > 0) {
            rbJobReady = 0;
            rbActOnJob();
        }
        return;
    }
    if (rbPollPending || (rbPolledOnce && rbElapsedMs(rbLastPollEnd, clock(), CLOCKS_PER_SEC) < (unsigned long)rbPollDelay))
        return;
    if ((count = rbEthLoadedCount()) <= 0)
        return;
    rbPollGames = count;
    rbPollPending = 1;
    if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &rbPoll) < 0)
        rbPollPending = 0;
}
