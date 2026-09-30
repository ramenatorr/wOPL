// Host tests for src/retrobox_parse.c (retrobox HTTP launch). Build and run from the repo root:
//   cc -std=c99 -Wall -Werror -I. -o /tmp/rb_parse_test tests/retrobox/test_parse.c src/retrobox_parse.c && /tmp/rb_parse_test
#include <stdio.h>
#include <string.h>
#include "include/retrobox_parse.h"

static int failures;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)

static void testConfig(void)
{
    rb_addr_t a;
    CHECK(rbParseConfig("brain = 192.168.1.20:8000\n", &a) == 0);
    CHECK(strcmp(a.ip, "192.168.1.20") == 0 && a.port == 8000);
    CHECK(rbParseConfig("# retrobox\n\n  brain=10.0.0.5:80  \r\n", &a) == 0);
    CHECK(strcmp(a.ip, "10.0.0.5") == 0 && a.port == 80);
    CHECK(rbParseConfig("\xEF\xBB\xBF"
                        "brain = 1.2.3.4:8000",
                        &a) == 0);
    CHECK(rbParseConfig("", &a) == -1);
    CHECK(rbParseConfig("brainx = 1.2.3.4:8000\n", &a) == -1);
    CHECK(rbParseConfig("brain = 1.2.3:8000\n", &a) == -1);
    CHECK(rbParseConfig("brain = 1.2.3.256:8000\n", &a) == -1);
    CHECK(rbParseConfig("brain = 1.2.3.4\n", &a) == -1);
    CHECK(rbParseConfig("brain = 1.2.3.4:0\n", &a) == -1);
    CHECK(rbParseConfig("brain = 1.2.3.4:65536\n", &a) == -1);
    CHECK(rbParseConfig("brain = retrobox.local:8000\n", &a) == -1);
}

static void testStatus(void)
{
    CHECK(rbParseStatus("HTTP/1.1 204 No Content\r\n\r\n") == 204);
    CHECK(rbParseStatus("HTTP/1.0 200 OK\r\n") == 200);
    CHECK(rbParseStatus("garbage") == -1);
    CHECK(rbParseStatus("") == -1);
}

static void testJob(void)
{
    rb_job_t j;
    const char *ok = "HTTP/1.1 200 OK\r\ncontent-type: text/plain; charset=utf-8\r\n\r\n"
                     "job=7f3a9c0e1b2d\nname=Ratchet & Clank: A=B\nmedia=DVD\n";
    CHECK(rbParseJob(ok, &j) == 0);
    CHECK(strcmp(j.id, "7f3a9c0e1b2d") == 0);
    CHECK(strcmp(j.name, "Ratchet & Clank: A=B") == 0);
    CHECK(j.media == RB_MEDIA_DVD);
    CHECK(rbParseJob("HTTP/1.1 200 OK\r\n\r\njob=a\nname=\xC5\x8C"
                     "kami\nmedia=CD\n",
                     &j) == 0);
    CHECK(strcmp(j.name, "\xC5\x8C"
                         "kami") == 0 &&
          j.media == RB_MEDIA_CD);
    CHECK(rbParseJob("HTTP/1.1 200 OK\r\n\r\njob=a\r\nname=B\r\nmedia=CD\r\n", &j) == 0);
    CHECK(strcmp(j.id, "a") == 0 && strcmp(j.name, "B") == 0);
    CHECK(rbParseJob("HTTP/1.1 200 OK\r\n\r\njob=a\nname=B\n", &j) == -1);            // no media
    CHECK(rbParseJob("HTTP/1.1 200 OK\r\n\r\njob=a\nname=B\nmedia=USB\n", &j) == -1); // bad media
    CHECK(rbParseJob("HTTP/1.1 200 OK\r\n\r\njob=\nname=B\nmedia=CD\n", &j) == -1);   // empty job
    CHECK(rbParseJob("HTTP/1.1 200 OK\r\njob=a\nname=B\nmedia=CD\n", &j) == -1);      // no body separator
    char longName[400] = "HTTP/1.1 200 OK\r\n\r\njob=a\nmedia=CD\nname=";
    memset(longName + strlen(longName), 'x', 161);
    CHECK(rbParseJob(longName, &j) == -1); // 161 > RB_NAME_MAX
}

static void testRequest(void)
{
    char buf[256];
    CHECK(rbFormatRequest(buf, sizeof(buf), "GET", "/opl/next?games=1756", "192.168.1.20:8000", NULL) > 0);
    CHECK(strcmp(buf, "GET /opl/next?games=1756 HTTP/1.0\r\nHost: 192.168.1.20:8000\r\n\r\n") == 0);
    CHECK(rbFormatRequest(buf, sizeof(buf), "POST", "/opl/claimed", "h:1", "job=a\n") > 0);
    CHECK(strcmp(buf, "POST /opl/claimed HTTP/1.0\r\nHost: h:1\r\nContent-Type: text/plain\r\n"
                      "Content-Length: 6\r\n\r\njob=a\n") == 0);
    CHECK(rbFormatRequest(buf, 16, "GET", "/opl/next?games=1756", "h:1", NULL) == -1); // too small
}

int main(void)
{
    testConfig();
    testStatus();
    testJob();
    testRequest();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
