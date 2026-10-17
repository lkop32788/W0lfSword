//  hub_log.c
//  W0lfSword Hub — L3.5: the app-owned log store behind the TweakLog hook.
//
//  See hub_log.h for why this exists and why it is pure C. The ring is one
//  fixed array under one mutex: `hub_log_append` runs on engine threads (the
//  hook is called for every TweakLog line, before the engine's own mutex
//  trylock), so it must be short and allocation-free.

#include "hub_log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_lines[HUB_LOG_LINES][HUB_LOG_LINE_MAX];
static int g_next;              // slot the next append writes to
static int g_count;             // lines stored (<= HUB_LOG_LINES)
static unsigned long g_total;   // lines ever appended
static unsigned long g_overwritten;
static unsigned long g_truncated;

// Oldest slot for the current ring state. Caller holds the mutex. g_next -
// g_count is in [-HUB_LOG_LINES, HUB_LOG_LINES), so adding the capacity once
// makes the modulo operand non-negative.
static int oldest_index(void)
{
    return (g_next - g_count + HUB_LOG_LINES) % HUB_LOG_LINES;
}

void hub_log_reset(void)
{
    pthread_mutex_lock(&g_mutex);
    memset(g_lines, 0, sizeof g_lines);
    g_next = 0;
    g_count = 0;
    g_total = 0;
    g_overwritten = 0;
    g_truncated = 0;
    pthread_mutex_unlock(&g_mutex);
}

void hub_log_append(const char *line)
{
    if (!line) return;

    // Strip the trailing newline(s) first: TweakLog callers pass formats both
    // with and without them, and a stored "\n" would export as an empty line.
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len == 0) return;

    int truncated = 0;
    if (len > HUB_LOG_LINE_MAX - 1) {
        len = HUB_LOG_LINE_MAX - 1;
        truncated = 1;
    }

    pthread_mutex_lock(&g_mutex);
    char *dst = g_lines[g_next];
    memcpy(dst, line, len);
    dst[len] = '\0';
    if (truncated) g_truncated++;
    g_next = (g_next + 1) % HUB_LOG_LINES;
    if (g_count < HUB_LOG_LINES) {
        g_count++;
    } else {
        g_overwritten++;    // the slot we just wrote held the oldest line
    }
    g_total++;
    pthread_mutex_unlock(&g_mutex);
}

void hub_log_hook_sink(const char *line)
{
    hub_log_append(line);
}

int hub_log_count(void)
{
    pthread_mutex_lock(&g_mutex);
    int n = g_count;
    pthread_mutex_unlock(&g_mutex);
    return n;
}

unsigned long hub_log_total(void)
{
    pthread_mutex_lock(&g_mutex);
    unsigned long n = g_total;
    pthread_mutex_unlock(&g_mutex);
    return n;
}

unsigned long hub_log_overwritten(void)
{
    pthread_mutex_lock(&g_mutex);
    unsigned long n = g_overwritten;
    pthread_mutex_unlock(&g_mutex);
    return n;
}

unsigned long hub_log_truncated(void)
{
    pthread_mutex_lock(&g_mutex);
    unsigned long n = g_truncated;
    pthread_mutex_unlock(&g_mutex);
    return n;
}

const char *hub_log_line(int index)
{
    const char *p = NULL;
    pthread_mutex_lock(&g_mutex);
    if (index >= 0 && index < g_count) {
        p = g_lines[(oldest_index() + index) % HUB_LOG_LINES];
    }
    pthread_mutex_unlock(&g_mutex);
    return p;
}

const char *hub_log_last(void)
{
    const char *p = NULL;
    pthread_mutex_lock(&g_mutex);
    if (g_count > 0) {
        p = g_lines[(oldest_index() + g_count - 1) % HUB_LOG_LINES];
    }
    pthread_mutex_unlock(&g_mutex);
    return p;
}

int hub_log_export(const char *path)
{
    if (!path || !path[0]) return -1;

    pthread_mutex_lock(&g_mutex);
    FILE *f = fopen(path, "w");
    if (!f) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    fprintf(f, "# W0lfSword Hub log store - %d lines, %lu total, %lu overwritten, %lu truncated\n",
            g_count, g_total, g_overwritten, g_truncated);
    int oldest = oldest_index();
    for (int i = 0; i < g_count; i++) {
        fprintf(f, "%s\n", g_lines[(oldest + i) % HUB_LOG_LINES]);
    }

    // One export per run: get it on disk before the app can be killed (the
    // 2026-09-11 panic lesson - the page cache does not survive a kernel panic).
    int rc = 0;
    if (fflush(f) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(f)) != 0) rc = -1;
    if (fclose(f) != 0) rc = -1;
    pthread_mutex_unlock(&g_mutex);
    return rc;
}
