// SPDX-License-Identifier: Apache-2.0
// Fresh-process timer of `tools/bench.py fresh`: native, so that nothing but posix_spawn and wait4
// is in the timed path.
//
//   fresh_timer RUNS WARMUP OUT BIN [ARG...] -- FILE...
//
// BIN is first written back, dropped from the page cache and read again: a binary copied a moment
// ago lies in the page cache in other pieces than one read back, which moves a run by tens of
// microseconds, and this way every binary under test starts the same. Then, for each FILE,
// `BIN --convert FILE OUT ARG...` runs WARMUP times untimed and RUNS times timed (posix_spawn to
// wait4, CLOCK_MONOTONIC; standard output and error to /dev/null). Per file: the median wall time,
// the median user + system CPU time and the median minor page faults of the RUNS runs. Prints one
// line of JSON: the median, p95 and mean of each over the files, and the number of runs that did
// not exit with 0.
#define _GNU_SOURCE
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static double Now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int Compare(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double Median(double* v, int n) {
    qsort(v, (size_t)n, sizeof(double), Compare);
    return v[n / 2];
}

static void Recache(const char* path) {
    static char block[1 << 20];
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    (void)fsync(fd);  // fails on a read-only mount, where nothing waits to be written
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    while (read(fd, block, sizeof block) > 0) {
    }
    close(fd);
}

static void Stats(const char* name, double* v, int n, const char* end) {
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += v[i];
    qsort(v, (size_t)n, sizeof(double), Compare);
    printf("\"%s\":{\"median\":%.4f,\"p95\":%.4f,\"mean\":%.4f}%s", name, v[n / 2], v[n * 95 / 100], sum / n, end);
}

int main(int argc, char** argv) {
    int sep = 5;
    while (sep < argc && strcmp(argv[sep], "--") != 0) ++sep;
    const int runs = argc > 1 ? atoi(argv[1]) : 0, warmup = argc > 2 ? atoi(argv[2]) : -1;
    const int files = argc - sep - 1, extra = sep - 5;
    if (runs < 1 || warmup < 0 || files < 1) {
        fprintf(stderr, "usage: fresh_timer RUNS WARMUP OUT BIN [ARG...] -- FILE...\n");
        return 2;
    }
    char* const out = argv[3];
    char* const bin = argv[4];
    Recache(bin);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char** args = calloc((size_t)extra + 5, sizeof *args);  // BIN --convert FILE OUT ARG... NULL
    args[0] = bin;
    args[1] = "--convert";
    args[3] = out;
    memcpy(args + 4, argv + 5, (size_t)extra * sizeof *args);
    double* wall = calloc((size_t)files, sizeof(double));
    double* cpu = calloc((size_t)files, sizeof(double));
    double* faults = calloc((size_t)files, sizeof(double));
    double* w = calloc((size_t)runs, sizeof(double));
    double* c = calloc((size_t)runs, sizeof(double));
    double* f = calloc((size_t)runs, sizeof(double));
    int failures = 0;
    for (int i = 0; i < files; ++i) {
        args[2] = argv[sep + 1 + i];
        for (int r = 0; r < warmup + runs; ++r) {
            const double start = Now();
            pid_t pid;
            if (posix_spawn(&pid, bin, &fa, NULL, args, environ) != 0) {
                fprintf(stderr, "fresh_timer: cannot start %s\n", bin);
                return 3;
            }
            int status = 0;
            struct rusage use;
            wait4(pid, &status, 0, &use);
            const double end = Now();
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) ++failures;
            if (r < warmup) continue;
            w[r - warmup] = (end - start) * 1e3;
            c[r - warmup] = (use.ru_utime.tv_sec + use.ru_stime.tv_sec) * 1e3 +
                            (use.ru_utime.tv_usec + use.ru_stime.tv_usec) * 1e-3;
            f[r - warmup] = (double)use.ru_minflt;
        }
        wall[i] = Median(w, runs);
        cpu[i] = Median(c, runs);
        faults[i] = Median(f, runs);
    }
    printf("{\"files\":%d,\"runs\":%d,\"failures\":%d,", files, runs, failures);
    Stats("wall_ms", wall, files, ",");
    Stats("cpu_ms", cpu, files, ",");
    Stats("minflt", faults, files, "}\n");
    return 0;
}
