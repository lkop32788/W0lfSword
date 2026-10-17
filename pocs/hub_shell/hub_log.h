#ifndef W0LFSWORD_HUB_LOG_H
#define W0LFSWORD_HUB_LOG_H

// L3.5 — app-owned log store for the W0lfSword Hub.
//
// The engine already writes every TweakLog line to four sinks (see
// utils/tweak_log.h): /tmp, the app-sandbox Documents copy, os_log and stderr.
// None of them is reachable from the app's own UI, and the app has no way to
// hand the lines to the host except through the engine's file naming.
//
// So this is the app side of the route:
//   * `hub_log_hook_sink` is passed to `tweak_log_set_hook()`, so every engine /
//     probe / shell line lands in a bounded in-memory ring (the app log store);
//   * `hub_log_export` writes that ring to a file the app owns, on demand — one
//     write per run, not a second continuous sink. Duplicating every line to
//     disk would double the I/O the engine's Documents copy already pays for,
//     and the BUG.6 lesson is that this app's log volume can spend a device's
//     whole daily disk budget.
//
// Pure C on purpose (no Foundation, no UIKit): it is host-testable, and the
// hook runs on engine threads, so it must not touch Objective-C objects.
//
// Wiring the hook also turns ON the engine sink's panic forensics: tweak_log.m
// fsyncs the Documents copy at most once per 200 ms only when a host app
// installed a hook (`tweak_log_hook_installed()`), which is what keeps a kernel
// panic from losing the tail of the run.

#ifdef __cplusplus
extern "C" {
#endif

// Ring capacity. 128 lines is enough to see the last stages of a run and small
// enough that the export is a single small write.
#define HUB_LOG_LINES 128
// Longest stored line, including the terminating NUL. Engine lines can be up to
// 2048 bytes; anything longer is truncated here (and counted) rather than
// dropped, so the ring can never be overrun by a long line.
#define HUB_LOG_LINE_MAX 256

// Clear the ring and the counters. Safe to call before any other function.
void hub_log_reset(void);

// Append one line. Thread-safe. A trailing "\r\n", "\n" or "\r" is stripped;
// NULL and empty lines are ignored. When the ring is full the OLDEST line is
// overwritten (and counted).
void hub_log_append(const char *line);

// tweak_log_hook_fn adapter: same as hub_log_append, void return so it can be
// handed to tweak_log_set_hook() directly.
void hub_log_hook_sink(const char *line);

// Lines currently stored (0 .. HUB_LOG_LINES).
int hub_log_count(void);
// Lines ever appended, including overwritten ones.
unsigned long hub_log_total(void);
// Lines lost to the ring being full (total - count, tracked explicitly).
unsigned long hub_log_overwritten(void);
// Lines shortened to HUB_LOG_LINE_MAX - 1.
unsigned long hub_log_truncated(void);

// Stored line by age: 0 = oldest, hub_log_count() - 1 = newest. NULL when the
// index is out of range. The pointer is valid until the next append/reset.
const char *hub_log_line(int index);
// Newest stored line, or NULL when the store is empty.
const char *hub_log_last(void);

// Write the stored lines to `path`, oldest first, with a header line naming the
// counts ("# W0lfSword Hub log store - 128 lines, 900 total, 772 overwritten").
// Returns 0 on success, -1 on a NULL/empty path or a file that cannot be
// opened. The file is truncated, so a re-export replaces the previous one.
int hub_log_export(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* W0LFSWORD_HUB_LOG_H */
