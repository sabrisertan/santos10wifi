/*
 * santos-proc-match -- authoritative process identity enumeration for the
 * private Pipeline candidate launcher.
 *
 * WHY THIS EXISTS
 * ---------------
 * The launcher used to enumerate /proc in shell:
 *
 *     for p in /proc/[0-9]*; do
 *       exe=$(readlink "$p/exe" 2>/dev/null) || continue
 *       ...
 *     done
 *
 * That is one fork() plus one exec() of /usr/bin/readlink PER PROCESS. On this
 * device the operator's physical Apps-grid launch measured that enumeration at
 * 17.48 s, and the launcher performed it three times before it would start the
 * app, so 52.5 s of a 58.7 s launch was fork overhead rather than any real work.
 * The same enumeration was 1.00 s on an idle system and 4.14 s warm, i.e. the
 * cost scaled with system load because it was paying for process creation.
 *
 * This helper performs the identical readlink() predicate, but in ONE process,
 * with no forks and no execs. The predicate is unchanged and remains
 * authoritative: the resolved target of /proc/<pid>/exe, compared as a string.
 * There is deliberately NO command-line or comm-based shortlist and NO name
 * matching. An instance is only ever reported or signalled when its RESOLVED
 * EXECUTABLE matches, so executable identity semantics are exactly what they were.
 *
 * PID REUSE
 * ---------
 * A pid alone is not an identity: between enumerating and signalling, the kernel
 * may recycle that pid for an unrelated process, and killing it would be a real
 * bug. Every reported identity therefore carries the process start time from
 * /proc/<pid>/stat field 22, and both `verify` and `signal` re-check pid AND
 * start time AND exe together before acting. A recycled pid fails the start-time
 * comparison and is left alone.
 *
 * EXIT STATUS
 * -----------
 *   scan / scan-exact   0 always (an empty result is not an error)
 *   verify              0 if every listed identity is still alive, else 1
 *   wait                0 if every listed identity exited before the timeout,
 *                       1 on timeout, 2 on usage error
 *   signal              0 if at least one signal was delivered, 1 if none was
 *                       (because every identity was already gone or had been
 *                       recycled), 2 on usage error
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Bounded on purpose. A pathological /proc must not be able to make this
 * unbounded, and a launcher must never be the reason a device stops responding. */
struct self_ids {
    long pid;
    long ppid;
};

#define MAX_PIDS 8192
#define MAX_IDENTITIES 256
#define PATH_MAX_ 4096

struct identity {
    long pid;
    long long starttime;
    char exe[PATH_MAX_];
};

/*
 * /proc/<pid>/stat field 22 is the process start time in clock ticks since boot.
 * The comm field (field 2) is parenthesised and may itself contain spaces AND
 * parentheses, so the line is split on the LAST ')' rather than parsing field 2
 * positionally. Everything after that ')' starts at field 3 (state), so starttime
 * is the 20th whitespace-separated token of the remainder.
 *
 * Returns 0 on success, -1 if the process vanished or the line is unparseable.
 */
static int read_starttime(const char *dir, long pid, long long *out)
{
    char path[64];
    char buf[4096];
    int fd;
    ssize_t n;
    char *last, *p;
    int field;

    snprintf(path, sizeof(path), "%s/%ld/stat", dir, pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    last = strrchr(buf, ')');
    if (!last)
        return -1;
    p = last + 1;
    /* p now points just past the comm field; the first token is field 3. */
    for (field = 3; field < 22; field++) {
        while (*p == ' ')
            p++;
        while (*p && *p != ' ')
            p++;
    }
    while (*p == ' ')
        p++;
    if (!*p)
        return -1;
    *out = strtoll(p, NULL, 10);
    return 0;
}

/* Resolved target of /proc/<pid>/exe. This is the authoritative predicate. */
static int read_exe(const char *dir, long pid, char *out, size_t outsz)
{
    char path[64];
    ssize_t n;

    snprintf(path, sizeof(path), "%s/%ld/exe", dir, pid);
    n = readlink(path, out, outsz - 1);
    if (n < 0)
        return -1;
    out[n] = '\0';
    return 0;
}

static int collect(const char *dir, const char *needle, int exact,
                   struct identity *out, int max)
{
    DIR *d;
    struct dirent *de;
    int n = 0, examined = 0;

    d = opendir(dir);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        char *end;
        long pid;
        char exe[PATH_MAX_];
        long long st;

        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        if (++examined > MAX_PIDS)
            break;
        pid = strtol(de->d_name, &end, 10);
        if (*end != '\0' || pid <= 0)
            continue;
        if (read_exe(dir, pid, exe, sizeof(exe)) != 0)
            continue;   /* kernel thread, or it exited: no resolved executable */
        if (needle) {
            if (exact) {
                if (strcmp(exe, needle) != 0)
                    continue;
            } else {
                if (strstr(exe, needle) == NULL)
                    continue;
            }
        }
        if (read_starttime(dir, pid, &st) != 0)
            continue;   /* cannot establish identity, so cannot report it */
        if (n >= max)
            break;
        out[n].pid = pid;
        out[n].starttime = st;
        snprintf(out[n].exe, sizeof(out[n].exe), "%s", exe);
        n++;
    }
    closedir(d);
    return n;
}

/*
 * Revalidate one recorded identity. Returns 1 only when the pid still exists,
 * its start time is unchanged (so it is the same process, not a recycled pid),
 * and its resolved executable still matches byte for byte.
 */
static int identity_alive(const char *dir, const struct identity *id)
{
    char exe[PATH_MAX_];
    long long st;

    if (read_exe(dir, id->pid, exe, sizeof(exe)) != 0)
        return 0;
    if (strcmp(exe, id->exe) != 0)
        return 0;
    if (read_starttime(dir, id->pid, &st) != 0)
        return 0;
    return st == id->starttime;
}

static void nap_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/*
 * The whole stale-instance reap in ONE invocation.
 *
 * This exists because the shell used to do it, and doing it in shell required
 * passing a list of identities through word splitting. That is exactly where the
 * first implementation of this went wrong: a newline-separated list of
 * "pid starttime exe" triples was fed through `set -- $ids` and unquoted
 * expansion, so each identity was torn into three separate words, identities were
 * duplicated because two substring scans matched the same process, and the
 * helper's own argv validation rejected the wait. None of that can happen when
 * the recording, signalling, waiting and escalation all live here.
 *
 * Sequence, per the accepted design:
 *   1. ONE enumeration, in this process, authoritative readlink predicate.
 *   2. Record every matched (pid, starttime, exe), de-duplicated.
 *   3. Never record this process or its parent.
 *   4. TERM exactly the recorded identities, revalidated.
 *   5. Poll ONLY the recorded identities, in-process, bounded.
 *   6. Only if the wait expires, KILL the still-alive recorded identities, each
 *      revalidated. No second enumeration of /proc, ever.
 */
static int usage(void);

static int cmd_reap(int argc, char **argv)
{
    struct identity ids[MAX_IDENTITIES];
    const char *needles[8];
    int nneedles = 0, i, j;
    long timeout_ms;
    int recorded = 0, signalled = 0, exited = 0, killed = 0, skipped = 0;
    struct self_ids self;

    if (argc < 3)
        return usage();
    timeout_ms = strtol(argv[2], NULL, 10);
    if (timeout_ms < 0)
        return usage();
    for (i = 3; i < argc && nneedles < 8; i++)
        needles[nneedles++] = argv[i];
    if (nneedles == 0) {
        needles[nneedles++] = "tubefeeder";
        needles[nneedles++] = "pipeline-santos";
    }

    self.pid = (long)getpid();
    self.ppid = (long)getppid();

    /*
     * DEFECT 1 FIX -- ONE traversal of /proc, every predicate tested per process.
     *
     * The previous version called collect() once per needle into the SAME ids[]
     * that held the recorded identities, so a match found by the second needle
     * overwrote an identity already recorded by the first. That is a real
     * correctness bug: an identity already recorded could be silently replaced,
     * so it would never be signalled. It also meant "one enumeration" was not
     * true -- it was one enumeration PER NEEDLE.
     *
     * Now /proc is walked exactly once. For each pid the resolved executable is
     * read once and tested against every accepted predicate, and the identity is
     * recorded at most once. De-duplication is therefore structural rather than
     * something a separate pass has to remember to do.
     */
    {
        DIR *d = opendir("/proc");
        struct dirent *de;
        int examined = 0;
        if (d == NULL) {
            printf("REAP_ERROR cannot-open-proc\n");
            return 1;
        }
        while ((de = readdir(d)) != NULL) {
            char *end;
            long pid;
            char exe[PATH_MAX_];
            long long st;
            int matched = 0;

            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            if (++examined > MAX_PIDS)
                break;
            pid = strtol(de->d_name, &end, 10);
            if (*end != '\0' || pid <= 0)
                continue;
            if (pid == self.pid || pid == self.ppid)
                continue;
            if (read_exe("/proc", pid, exe, sizeof(exe)) != 0)
                continue;
            for (i = 0; i < nneedles; i++) {
                if (strstr(exe, needles[i]) != NULL) {
                    matched = 1;
                    break;
                }
            }
            if (!matched)
                continue;
            if (read_starttime("/proc", pid, &st) != 0)
                continue;   /* cannot establish identity, so cannot act on it */
            if (recorded >= MAX_IDENTITIES)
                break;
            ids[recorded].pid = pid;
            ids[recorded].starttime = st;
            snprintf(ids[recorded].exe, sizeof(ids[recorded].exe), "%s", exe);
            recorded++;
        }
        closedir(d);
    }
    (void)j;

    for (i = 0; i < recorded; i++)
        printf("RECORDED %ld %lld %s\n", ids[i].pid, ids[i].starttime, ids[i].exe);
    printf("RECORDED_COUNT %d\n", recorded);
    fflush(stdout);

    if (recorded == 0) {
        printf("REAP recorded=0 signalled=0 exited=0 killed=0 skipped=0\n");
        return 0;
    }

    for (i = 0; i < recorded; i++) {
        if (!identity_alive("/proc", &ids[i])) {
            printf("SIGNAL %ld %lld %s skipped-gone-or-reused\n",
                   ids[i].pid, ids[i].starttime, ids[i].exe);
            skipped++;
            continue;
        }
        if (kill((pid_t)ids[i].pid, SIGTERM) == 0) {
            printf("SIGNAL %ld %lld %s signalled-TERM\n",
                   ids[i].pid, ids[i].starttime, ids[i].exe);
            signalled++;
        } else {
            printf("SIGNAL %ld %lld %s signal-failed-TERM\n",
                   ids[i].pid, ids[i].starttime, ids[i].exe);
            skipped++;
        }
    }
    fflush(stdout);

    /* Poll ONLY the recorded identities, in this process, bounded. */
    {
        long waited = 0;
        const long step = 100;
        for (;;) {
            int alive = 0;
            for (i = 0; i < recorded; i++)
                if (identity_alive("/proc", &ids[i]))
                    alive = 1;
            if (!alive)
                break;
            if (waited >= timeout_ms)
                break;
            nap_ms(step);
            waited += step;
        }
    }

    for (i = 0; i < recorded; i++) {
        if (!identity_alive("/proc", &ids[i]))
            exited++;
    }

    /* Escalate only for identities still alive after the bounded wait, and only
     * after revalidating pid AND starttime AND exe. */
    if (exited < recorded) {
        for (i = 0; i < recorded; i++) {
            if (!identity_alive("/proc", &ids[i]))
                continue;
            if (kill((pid_t)ids[i].pid, SIGKILL) == 0) {
                printf("KILL %ld %lld %s signalled-KILL\n",
                       ids[i].pid, ids[i].starttime, ids[i].exe);
                killed++;
            }
        }
        {
            long waited = 0;
            for (;;) {
                int alive = 0;
                for (i = 0; i < recorded; i++)
                    if (identity_alive("/proc", &ids[i]))
                        alive = 1;
                if (!alive || waited >= 2000)
                    break;
                nap_ms(100);
                waited += 100;
            }
        }
    }

    printf("REAP recorded=%d signalled=%d exited=%d killed=%d skipped=%d\n",
           recorded, signalled, exited, killed, skipped);
    return 0;
}

static int usage(void);

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  santos-proc-match scan <substring>\n"
        "  santos-proc-match scan-exact <path>\n"
        "  santos-proc-match verify  <pid> <starttime> <exe> ...\n"
        "  santos-proc-match wait <timeout_ms> <pid> <starttime> <exe> ...\n"
        "  santos-proc-match signal <TERM|KILL> <pid> <starttime> <exe> ...\n"
        "  santos-proc-match reap <timeout_ms> [substring ...]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *dir = "/proc";
    struct identity ids[MAX_IDENTITIES];
    int i, n;

    if (argc < 2)
        return usage();

    if (strcmp(argv[1], "reap") == 0)
        return cmd_reap(argc, argv);

    if (strcmp(argv[1], "scan") == 0 || strcmp(argv[1], "scan-exact") == 0) {
        if (argc != 3)
            return usage();
        n = collect(dir, argv[2], strcmp(argv[1], "scan-exact") == 0,
                    ids, MAX_IDENTITIES);
        if (n < 0)
            return 0;   /* cannot open /proc: report nothing rather than guess */
        for (i = 0; i < n; i++)
            printf("%ld %lld %s\n", ids[i].pid, ids[i].starttime, ids[i].exe);
        return 0;
    }

    if (strcmp(argv[1], "verify") == 0 || strcmp(argv[1], "wait") == 0) {
        int all_alive = 1;
        long timeout_ms = 0;

        if (strcmp(argv[1], "wait") == 0) {
            if (argc < 3)
                return usage();
            timeout_ms = strtol(argv[2], NULL, 10);
            if (timeout_ms < 0)
                return usage();
            argv += 2;
            argc -= 2;
        } else {
            argv += 1;
            argc -= 1;
        }
        if (argc < 4 || ((argc - 1) % 3) != 0)
            return usage();

        n = 0;
        for (i = 0; i + 2 < argc; i += 3) {
            if (n >= MAX_IDENTITIES)
                return usage();
            ids[n].pid = strtol(argv[i + 1], NULL, 10);
            ids[n].starttime = strtoll(argv[i + 2], NULL, 10);
            snprintf(ids[n].exe, sizeof(ids[n].exe), "%s", argv[i + 3]);
            n++;
        }

        if (strcmp(argv[1], "wait") == 0) {
            long waited = 0;
            const long step = 100;
            for (;;) {
                int alive = 0;
                for (i = 0; i < n; i++) {
                    if (identity_alive(dir, &ids[i])) {
                        alive = 1;
                        all_alive = 0;
                    }
                }
                if (!alive)
                    return 0;
                if (waited >= timeout_ms)
                    return 1;
                nap_ms(step);
                waited += step;
            }
        }

        for (i = 0; i < n; i++) {
            int alive = identity_alive(dir, &ids[i]);
            printf("%ld %lld %s\n", ids[i].pid, ids[i].starttime,
                   alive ? "alive" : "gone-or-reused");
            if (!alive)
                all_alive = 0;
        }
        return all_alive ? 0 : 1;
    }

    if (strcmp(argv[1], "signal") == 0) {
        int sig, delivered = 0;

        if (argc < 3)
            return usage();
        if (strcmp(argv[2], "TERM") == 0)
            sig = SIGTERM;
        else if (strcmp(argv[2], "KILL") == 0)
            sig = SIGKILL;
        else
            return usage();

        argv += 2;
        argc -= 2;
        if (argc < 4 || ((argc - 1) % 3) != 0)
            return usage();

        n = 0;
        for (i = 0; i + 2 < argc; i += 3) {
            if (n >= MAX_IDENTITIES)
                return usage();
            ids[n].pid = strtol(argv[i + 1], NULL, 10);
            ids[n].starttime = strtoll(argv[i + 2], NULL, 10);
            snprintf(ids[n].exe, sizeof(ids[n].exe), "%s", argv[i + 3]);
            n++;
        }

        for (i = 0; i < n; i++) {
            /* Revalidate pid AND starttime AND exe before every signal. A pid
             * recycled between enumeration and here is skipped, not killed. */
            if (!identity_alive(dir, &ids[i])) {
                printf("%ld %lld %s skipped-gone-or-reused\n",
                       ids[i].pid, ids[i].starttime, ids[i].exe);
                continue;
            }
            if (kill((pid_t)ids[i].pid, sig) == 0) {
                printf("%ld %lld %s signalled-%s\n",
                       ids[i].pid, ids[i].starttime, ids[i].exe,
                       sig == SIGTERM ? "TERM" : "KILL");
                delivered++;
            } else {
                printf("%ld %lld %s signal-failed-%s\n",
                       ids[i].pid, ids[i].starttime, ids[i].exe,
                       sig == SIGTERM ? "TERM" : "KILL");
            }
        }
        return delivered > 0 ? 0 : 1;
    }

    return usage();
}
