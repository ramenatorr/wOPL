#ifndef RETROBOX_PARSE_H
#define RETROBOX_PARSE_H

// retrobox: pure parsing/formatting for the brain's HTTP launch (no PS2 headers, host-testable:
// tests/retrobox/test_parse.c).

#define RB_MEDIA_CD 0x12 // SCECdPS2CD

#define RB_MEDIA_DVD 0x14 // SCECdPS2DVD

#define RB_JOB_ID_MAX 32

#define RB_NAME_MAX 160 // ISO_GAME_NAME_MAX

typedef struct
{
    char ip[16];
    int port;
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
int rbFormatRequest(char *buf, int len, const char *method, const char *path, const char *host, const char *body);

#endif
