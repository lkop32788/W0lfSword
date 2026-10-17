//
//  hub_log_host_test.c
//  W0lfSword — host test for L3.5: the Hub app's log store (hub_log.c).
//
//  hub_log.c is the app side of the TweakLog plumbing: the app installs
//  hub_log_hook_sink() through tweak_log_set_hook(), so every engine/probe line
//  lands in a bounded ring that the app can display and export. None of that
//  needs a device, and the failure modes are all off-by-ones, so it is tested
//  here against the REAL file the app compiles (no stub copy):
//
//    1. the empty store (no line, no count, no last line);
//    2. ordering and the index contract (0 = oldest, count-1 = newest, out of
//       range = NULL, negative = NULL);
//    3. newline handling - a trailing "\n"/"\r\n" is stripped, an all-newline
//       line is ignored rather than stored as an empty entry;
//    4. wraparound: past capacity the OLDEST line goes, and the overwritten
//       counter names how many left (a silent overwrite is how a ring loses the
//       beginning of a run without anyone noticing);
//    5. a line longer than the slot is truncated, counted, and still
//       NUL-terminated - the ring must never be overrun by a 2 KB engine line;
//    6. export: header + oldest-first order, file truncated on re-export, and
//       -1 for a NULL/empty path and for a file that cannot be opened;
//    7. the thread contract: the hook is called from engine threads, so two
//       threads appending concurrently must not lose a line or tear the ring.
//
//  No device, no kernel, no network.
//
//  Run: bash scripts/run_hub_log_host_test.sh
//
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hub_log.h"

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

static void check(int ok, const char *what)
{
    g_checks++;
    if (ok) {
        printf("  ok   %s\n", what);
    } else {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

// Compare a stored line with an expected string, tolerating NULL.
static void check_line(int index, const char *want, const char *what)
{
    const char *got = hub_log_line(index);
    check(got != NULL && strcmp(got, want) == 0, what);
    if (got == NULL || strcmp(got, want) != 0) {
        printf("       line[%d] = %s (want \"%s\")\n", index,
               got ? got : "(null)", want);
    }
}

// ---------------------------------------------------------------------------
// 1. empty store
// ---------------------------------------------------------------------------
static void test_empty(void)
{
    hub_log_reset();
    check(hub_log_count() == 0, "fresh store holds no lines");
    check(hub_log_total() == 0, "fresh store counts no appends");
    check(hub_log_overwritten() == 0, "fresh store counts no overwrites");
    check(hub_log_line(0) == NULL, "line(0) is NULL on a fresh store");
    check(hub_log_last() == NULL, "last() is NULL on a fresh store");
}

// ---------------------------------------------------------------------------
// 2. ordering + the index contract
// ---------------------------------------------------------------------------
static void test_ordering(void)
{
    hub_log_reset();
    hub_log_append("alpha");
    hub_log_append("beta");
    hub_log_append("gamma");

    check(hub_log_count() == 3, "three appends store three lines");
    check(hub_log_total() == 3, "three appends count as three");
    check(hub_log_overwritten() == 0, "a ring that is not full overwrites nothing");
    check_line(0, "alpha", "index 0 is the OLDEST line");
    check_line(1, "beta", "index 1 follows");
    check_line(2, "gamma", "index 2 is the newest");
    check(hub_log_line(3) == NULL, "an index past the newest is NULL, not a wrap");
    check(hub_log_line(-1) == NULL, "a negative index is NULL");
    check(hub_log_last() != NULL && strcmp(hub_log_last(), "gamma") == 0,
          "last() follows the newest line");
}

// ---------------------------------------------------------------------------
// 3. newline handling
// ---------------------------------------------------------------------------
static void test_newlines(void)
{
    hub_log_reset();
    hub_log_append("with lf\n");
    hub_log_append("with crlf\r\n");
    hub_log_append("plain");

    check(hub_log_count() == 3, "a trailing newline does not create an extra entry");
    check_line(0, "with lf", "a trailing \\n is stripped");
    check_line(1, "with crlf", "a trailing \\r\\n is stripped");

    int before = hub_log_count();
    unsigned long total_before = hub_log_total();
    hub_log_append(NULL);
    hub_log_append("");
    hub_log_append("\n");
    hub_log_append("\r\n");
    check(hub_log_count() == before, "NULL / empty / newline-only lines are not stored");
    check(hub_log_total() == total_before, "and they are not counted as appends");
}

// ---------------------------------------------------------------------------
// 4. wraparound
// ---------------------------------------------------------------------------
static void test_wraparound(void)
{
    hub_log_reset();
    char buf[64];
    for (int i = 0; i < HUB_LOG_LINES + 2; i++) {
        snprintf(buf, sizeof buf, "line-%d", i);
        hub_log_append(buf);
    }

    check(hub_log_count() == HUB_LOG_LINES, "the ring stops at its capacity");
    check(hub_log_total() == (unsigned long)(HUB_LOG_LINES + 2),
          "every append is still counted");
    check(hub_log_overwritten() == 2, "the two lines pushed out are counted");
    check_line(0, "line-2", "the oldest surviving line is the third append");
    check_line(HUB_LOG_LINES - 1, "line-129",
               "the newest line sits at count - 1 after a wrap");

    // Rebuild the expected sequence for the full ring: line-2 .. line-129.
    int ordered = 1;
    for (int i = 0; i < HUB_LOG_LINES; i++) {
        snprintf(buf, sizeof buf, "line-%d", i + 2);
        const char *got = hub_log_line(i);
        if (!got || strcmp(got, buf) != 0) {
            ordered = 0;
            printf("       out of order at %d: %s (want %s)\n", i,
                   got ? got : "(null)", buf);
            break;
        }
    }
    check(ordered, "the whole ring stays oldest-first across a wrap");
}

// ---------------------------------------------------------------------------
// 5. truncation
// ---------------------------------------------------------------------------
static void test_truncation(void)
{
    hub_log_reset();
    char big[4096];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    hub_log_append(big);

    const char *got = hub_log_last();
    check(got != NULL, "an over-long line is still stored");
    check(got != NULL && strlen(got) == HUB_LOG_LINE_MAX - 1,
          "it is cut to the slot, not written past it");
    check(hub_log_truncated() == 1, "the truncation is counted");
    check(got != NULL && got[0] == 'x' && got[HUB_LOG_LINE_MAX - 2] == 'x',
          "the kept prefix is the start of the original line");

    // A short line after a truncated one must be intact, i.e. the slot's
    // terminator is written per append, not inherited from the long line.
    hub_log_append("short");
    check_line(1, "short", "a short line after a truncated one is whole");
}

// ---------------------------------------------------------------------------
// 6. export
// ---------------------------------------------------------------------------
static char g_dir[256];

// Read a whole file into buf. Returns bytes read, or -1.
static long read_file(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

static void test_export(void)
{
    hub_log_reset();
    hub_log_append("one");
    hub_log_append("two");
    hub_log_append("three");

    char path[512];
    snprintf(path, sizeof path, "%s/hub.log", g_dir);
    int rc = hub_log_export(path);
    check(rc == 0, "export writes the ring to a file");

    char buf[8192];
    long n = read_file(path, buf, sizeof buf);
    check(n > 0, "the exported file is readable and non-empty");
    check(strstr(buf, "# W0lfSword Hub log store - 3 lines, 3 total") != NULL,
          "the header names the stored / total counts");
    check(strstr(buf, "one\ntwo\nthree\n") != NULL,
          "the body is oldest-first, one line per entry");
    check(strstr(buf, "\n\n") == NULL, "the export has no blank filler lines");

    // Re-export truncates instead of appending a second copy.
    rc = hub_log_export(path);
    n = read_file(path, buf, sizeof buf);
    check(rc == 0 && n > 0, "a second export succeeds");
    const char *second = strstr(buf, "one");
    check(second != NULL && strstr(second + 1, "one") == NULL,
          "a re-export replaces the file, it does not append to it");

    // Wrapped ring: the file must start at the oldest SURVIVOR.
    hub_log_reset();
    char line[32];
    for (int i = 0; i < HUB_LOG_LINES + 2; i++) {
        snprintf(line, sizeof line, "w-%d", i);
        hub_log_append(line);
    }
    check(hub_log_export(path) == 0, "export works on a wrapped ring");
    n = read_file(path, buf, sizeof buf);
    check(n > 0 && strstr(buf, "w-2\n") != NULL && strstr(buf, "w-1\n") == NULL,
          "a wrapped export starts at the oldest surviving line");
    check(n > 0 && strstr(buf, "w-129\n") != NULL, "and ends at the newest");

    // Refusals.
    check(hub_log_export(NULL) == -1, "a NULL path is refused");
    check(hub_log_export("") == -1, "an empty path is refused");
    snprintf(path, sizeof path, "%s/no/such/dir/hub.log", g_dir);
    check(hub_log_export(path) == -1, "an unopenable path returns -1, not a crash");
}

// ---------------------------------------------------------------------------
// 7. the thread contract
// ---------------------------------------------------------------------------
typedef struct {
    const char *prefix;
    int count;
} writer_job_t;

static void *writer(void *arg)
{
    writer_job_t *job = (writer_job_t *)arg;
    char buf[64];
    for (int i = 0; i < job->count; i++) {
        snprintf(buf, sizeof buf, "%s-%d", job->prefix, i);
        hub_log_hook_sink(buf);     // the app installs this exact function
    }
    return NULL;
}

static void test_threads(void)
{
    hub_log_reset();
    writer_job_t a = { "tA", 500 };
    writer_job_t b = { "tB", 500 };
    pthread_t ta, tb;
    pthread_create(&ta, NULL, writer, &a);
    pthread_create(&tb, NULL, writer, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    check(hub_log_total() == 1000, "two threads appending 500 lines each lose none");
    check(hub_log_count() == HUB_LOG_LINES, "the ring is full afterwards");
    check(hub_log_overwritten() == (unsigned long)(1000 - HUB_LOG_LINES),
          "every line pushed out is accounted for");

    // The last line is whichever thread finished last, so accept either - but
    // it must be a complete line, not a torn one.
    const char *last = hub_log_last();
    check(last != NULL && (strcmp(last, "tA-499") == 0 || strcmp(last, "tB-499") == 0),
          "the newest line is one of the two final appends, torn-free");
}

int main(void)
{
    printf("L3.5 host test - the Hub app log store (hub_log.c)\n\n");

    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/hub_log_test.XXXXXX", (tmp && tmp[0]) ? tmp : "/tmp");
    if (!mkdtemp(g_dir)) {
        printf("  FAIL could not create a scratch dir under %s\n", g_dir);
        printf("\nchecks=1 failures=1\nHUB_LOG_HOST_TEST FAIL\n");
        return 1;
    }

    test_empty();
    test_ordering();
    test_newlines();
    test_wraparound();
    test_truncation();
    test_export();
    test_threads();

    // Leave nothing behind.
    char path[512];
    snprintf(path, sizeof path, "%s/hub.log", g_dir);
    unlink(path);
    rmdir(g_dir);

    printf("\nchecks=%d failures=%d\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("HUB_LOG_HOST_TEST PASS\n");
        return 0;
    }
    printf("HUB_LOG_HOST_TEST FAIL\n");
    return 1;
}
