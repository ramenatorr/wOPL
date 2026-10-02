#ifndef RETROBOX_PARSE_H
#define RETROBOX_PARSE_H

// retrobox: pure parsing/formatting for the brain's HTTP launch (no PS2 headers, host-testable:
// tests/retrobox/test_parse.c).

#define RB_MEDIA_CD 0x12 // SCECdPS2CD

#define RB_MEDIA_DVD 0x14 // SCECdPS2DVD

#define RB_JOB_ID_MAX 32

#define RB_NAME_MAX 160 // ISO_GAME_NAME_MAX

#define RB_POLL_EVERY_MS 2000

#define RB_POLL_BACKOFF_MS 10000 // after a failed exchange: a brain that is down is not hammered

#define RB_JOB_MAX_AGE_MS 10000 // a job not acted on by then is dropped: the brain gives up at 15 s

typedef struct
{
    char ip[16];
    unsigned char octet[4]; // network order, for sin_addr (inet_addr() is dead in wOPL: no EE-side lwIP)
    int port;
    int trace; // "trace = 1": write the step trace to <share>retrobox.log
} rb_addr_t;

typedef struct
{
    char id[RB_JOB_ID_MAX + 1];
    char name[RB_NAME_MAX + 1];
    unsigned char media;
} rb_job_t;

int rbParseConfig(const char *text, rb_addr_t *out);
int rbParseStatus(const char *resp);
int rbParseJob(const char *resp, rb_job_t *job);
unsigned long rbElapsedMs(unsigned long start, unsigned long now, unsigned long perSec);
int rbPollDelayMs(int lastExchangeOk);
int rbJobFresh(unsigned long ageMs);
int rbResponseComplete(const char *buf, int len);
int rbFormatRequest(char *buf, int len, const char *method, const char *path, const char *host, const char *body);
int rbCmpNameExt(const char *name, int nameMax, const char *ext, const char *filename);

#endif
