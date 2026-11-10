// SPDX-License-Identifier: Apache-2.0
// Fresh-process timer of `tools/bench.py tools` and `pages`: one command, start to exit.
//
//   cmdbench RUNS WARMUP -- PROGRAM ARG...
//
// Runs the command WARMUP times untimed and RUNS times timed, each as a new process (posix_spawnp
// to wait4, CLOCK_MONOTONIC; standard output and error to /dev/null), and prints one line of JSON:
//   {"runs":N,"median_ms":..,"min_ms":..,"max_ms":..,"max_rss_kib":..,"max_rss_desc_kib":..,"failures":..}
// max_rss_kib is the largest peak resident set of the started process over the timed runs,
// max_rss_desc_kib the largest of any process of any run, started ones' children included (a JVM
// behind a launcher), failures the number of runs that did not exit with 0. Built static by
// tools/bench.py, so that the same program runs in every tool's image.
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

extern char** environ;

static double Now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int Compare(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
    const int runs = argc > 1 ? atoi(argv[1]) : 0, warmup = argc > 2 ? atoi(argv[2]) : -1;
    if (argc < 5 || strcmp(argv[3], "--") != 0 || runs < 1 || warmup < 0) {
        fprintf(stderr, "usage: cmdbench RUNS WARMUP -- PROGRAM ARG...\n");
        return 2;
    }
    char** args = argv + 4;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    double* times = calloc((size_t)runs, sizeof(double));
    long maxRss = 0;
    int failures = 0;
    for (int i = 0; i < warmup + runs; ++i) {
        pid_t pid;
        const double start = Now();
        if (posix_spawnp(&pid, args[0], &fa, NULL, args, environ) != 0) {
            fprintf(stderr, "cmdbench: cannot start %s\n", args[0]);
            return 3;
        }
        int status = 0;
        struct rusage usage;
        wait4(pid, &status, 0, &usage);
        const double end = Now();
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failures++;
        if (i >= warmup) {
            times[i - warmup] = end - start;
            if (usage.ru_maxrss > maxRss) maxRss = usage.ru_maxrss;
        }
    }
    qsort(times, (size_t)runs, sizeof(double), Compare);
    const double median = runs % 2 ? times[runs / 2] : (times[runs / 2 - 1] + times[runs / 2]) / 2;
    struct rusage all;
    getrusage(RUSAGE_CHILDREN, &all);
    printf("{\"runs\":%d,\"median_ms\":%.3f,\"min_ms\":%.3f,\"max_ms\":%.3f,\"max_rss_kib\":%ld,"
           "\"max_rss_desc_kib\":%ld,\"failures\":%d}\n",
           runs, median, times[0], times[runs - 1], maxRss, all.ru_maxrss, failures);
    return 0;
}
