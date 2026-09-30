// retrobox: parsing and formatting for the brain's HTTP launch (retro-gaming-box-poc
// docs/superpowers/specs/2026-09-30-ps2-opl-http-launch-design.md). No PS2 dependencies.
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "include/retrobox_parse.h"

static const char *rbSkipSpace(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int rbValidIp(const char *ip)
{
    int parts = 0, value = -1;
    for (;; ip++) {
        if (*ip >= '0' && *ip <= '9') {
            value = (value < 0 ? 0 : value) * 10 + (*ip - '0');
            if (value > 255)
                return 0;
        } else if (*ip == '.' || *ip == '\0') {
            if (value < 0)
                return 0;
            parts++;
            value = -1;
            if (*ip == '\0')
                break;
        } else
            return 0;
    }
    return parts == 4;
}

static int rbParseBrainLine(const char *p, const char *end, rb_addr_t *out)
{
    char ip[16];
    int n = 0, digits = 0;
    long port = 0;

    p = rbSkipSpace(p);
    if (end - p < 5 || strncmp(p, "brain", 5) != 0)
        return -1;
    p = rbSkipSpace(p + 5);
    if (p >= end || *p != '=')
        return -1;
    p = rbSkipSpace(p + 1);
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.')) {
        if (n >= 15)
            return -1;
        ip[n++] = *p++;
    }
    ip[n] = '\0';
    if (p >= end || *p != ':' || !rbValidIp(ip))
        return -1;
    p++;
    while (p < end && *p >= '0' && *p <= '9') {
        port = port * 10 + (*p++ - '0');
        if (++digits > 5)
            return -1;
    }
    p = rbSkipSpace(p);
    if (digits == 0 || port < 1 || port > 65535 || (p < end && *p != '\r' && *p != '#'))
        return -1;
    strcpy(out->ip, ip);
    out->port = (int)port;
    {
        const char *q = ip;
        int i;
        for (i = 0; i < 4; i++) {
            int v = 0;
            while (*q >= '0' && *q <= '9')
                v = v * 10 + (*q++ - '0');
            out->octet[i] = (unsigned char)v;
            if (*q == '.')
                q++;
        }
    }
    return 0;
}

static int rbParseTraceLine(const char *p, const char *end, int *trace)
{
    p = rbSkipSpace(p);
    if (end - p < 5 || strncmp(p, "trace", 5) != 0)
        return -1;
    p = rbSkipSpace(p + 5);
    if (p >= end || *p != '=')
        return -1;
    p = rbSkipSpace(p + 1);
    if (p >= end || (*p != '0' && *p != '1'))
        return -1;
    *trace = *p - '0';
    return 0;
}

int rbParseConfig(const char *text, rb_addr_t *out)
{
    const char *line = text;
    int found = 0;

    if (strncmp(line, "\xEF\xBB\xBF", 3) == 0)
        line += 3;

    out->trace = 0;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end)
            end = line + strlen(line);
        if (!found && rbParseBrainLine(line, end, out) == 0)
            found = 1;
        else
            rbParseTraceLine(line, end, &out->trace);
        line = *end ? end + 1 : end;
    }
    return found ? 0 : -1;
}

int rbParseStatus(const char *resp)
{
    int code = 0, i;

    if (strncmp(resp, "HTTP/1.", 7) != 0 || resp[7] == '\0' || resp[8] != ' ')
        return -1;
    for (i = 9; i < 12; i++) {
        if (resp[i] < '0' || resp[i] > '9')
            return -1;
        code = code * 10 + (resp[i] - '0');
    }
    return code;
}

static int rbCopyValue(char *dst, int max, const char *v, const char *end)
{
    int len = (int)(end - v);

    if (len > 0 && v[len - 1] == '\r')
        len--;
    if (len <= 0 || len > max)
        return -1;
    memcpy(dst, v, len);
    dst[len] = '\0';
    return 0;
}

int rbParseJob(const char *resp, rb_job_t *job)
{
    const char *p = strstr(resp, "\r\n\r\n");
    char media[4];
    int have = 0;

    if (!p)
        return -1;
    p += 4;
    while (*p) {
        const char *end = strchr(p, '\n'), *eq;
        if (!end)
            end = p + strlen(p);
        eq = memchr(p, '=', end - p);
        if (eq) {
            int klen = (int)(eq - p);
            if (klen == 3 && strncmp(p, "job", 3) == 0) {
                if (rbCopyValue(job->id, RB_JOB_ID_MAX, eq + 1, end))
                    return -1;
                have |= 1;
            } else if (klen == 4 && strncmp(p, "name", 4) == 0) {
                if (rbCopyValue(job->name, RB_NAME_MAX, eq + 1, end))
                    return -1;
                have |= 2;
            } else if (klen == 5 && strncmp(p, "media", 5) == 0) {
                if (rbCopyValue(media, 3, eq + 1, end))
                    return -1;
                have |= 4;
            }
        }
        p = *end ? end + 1 : end;
    }
    if (have != 7)
        return -1;
    if (strcmp(media, "CD") == 0)
        job->media = RB_MEDIA_CD;
    else if (strcmp(media, "DVD") == 0)
        job->media = RB_MEDIA_DVD;
    else
        return -1;
    return 0;
}

int rbFormatRequest(char *buf, int len, const char *method, const char *path, const char *host, const char *body)
{
    int n;

    if (body)
        n = snprintf(buf, len, "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s",
                     method, path, host, (int)strlen(body), body);
    else
        n = snprintf(buf, len, "%s %s HTTP/1.0\r\nHost: %s\r\n\r\n", method, path, host);
    return (n < 0 || n >= len) ? -1 : n;
}

// clock() ticks since start, in ms. clock_t is 32-bit unsigned on the EE (CLOCKS_PER_SEC 1000000, wraps every ~71.6
// min), so the difference is taken modulo 2^32 and converted in 64 bits.
unsigned long rbElapsedMs(unsigned long start, unsigned long now, unsigned long perSec)
{
    unsigned int ticks = (unsigned int)now - (unsigned int)start;
    return (unsigned long)(((unsigned long long)ticks * 1000ULL) / perSec);
}

int rbPollDelayMs(int lastExchangeOk)
{
    return lastExchangeOk ? RB_POLL_EVERY_MS : RB_POLL_BACKOFF_MS;
}

int rbJobFresh(unsigned long ageMs)
{
    return ageMs <= RB_JOB_MAX_AGE_MS;
}

// 1 once buf holds a whole reply: headers plus, for a 204/304, nothing; otherwise Content-Length body bytes. 0 while
// more is due, including a reply without Content-Length (read to close or deadline).
int rbResponseComplete(const char *buf, int len)
{
    const char *end, *p;
    int status, body;
    long clen = -1;

    if (len < 12)
        return 0;
    end = strstr(buf, "\r\n\r\n");
    if (!end || end - buf + 4 > len)
        return 0;
    status = rbParseStatus(buf);
    if (status == 204 || status == 304)
        return 1;
    for (p = buf; p < end; p++) {
        if ((p == buf || p[-1] == '\n') && strncasecmp(p, "content-length:", 15) == 0) {
            const char *q = p + 15;
            while (*q == ' ' || *q == '\t')
                q++;
            clen = 0;
            while (*q >= '0' && *q <= '9')
                clen = clen * 10 + (*q++ - '0');
            break;
        }
    }
    if (clen < 0)
        return 0;
    body = len - (int)(end - buf + 4);
    return body >= clen;
}
