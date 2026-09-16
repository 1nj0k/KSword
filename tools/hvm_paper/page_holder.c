/* Hold a real file-cache page resident and report its observed guest PFN.
 * mlock prevents reclamation, not every possible migration: each sample rechecks
 * pagemap and the experiment must reject any PFN change. No kernel modification. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char prefix[] = "{\"product\":\"research-fixture\",\"price\":1250,\"padding\":\"";
    uint64_t first = 0, entry = 0;
    char *page;
    int fd, pm, i, duration;
    if (argc != 3 || (duration = atoi(argv[2])) < 1 || duration > 1800) return 2;
    if (sysconf(_SC_PAGESIZE) != 4096) return 3;
    fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0 || ftruncate(fd, 4096)) { perror("create"); return 4; }
    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 5; }
    memset(page, ' ', 4096);
    memcpy(page, prefix, sizeof(prefix) - 1);
    memcpy(page + 4093, "\"}\n", 3);
    if (msync(page, 4096, MS_SYNC) || mlock(page, 4096)) { perror("resident"); return 6; }
    pm = open("/proc/self/pagemap", O_RDONLY);
    if (pm < 0) { perror("pagemap"); return 7; }
    for (i = 0; i <= duration; ++i) {
        struct timespec now;
        uint64_t pfn;
        if (pread(pm, &entry, 8, (off_t)((uintptr_t)page / 4096 * 8)) != 8) return 8;
        pfn = entry & ((1ULL << 55) - 1);
        if (!(entry & (1ULL << 63)) || (entry & (1ULL << 62)) || !pfn) return 9;
        if (i == 0) first = pfn;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (i % 2 == 0 || pfn != first) {
            printf("{\"kind\":\"application-page\",\"pid\":%d,\"gpa\":\"0x%llx\",\"samePfn\":%s,\"uptime\":%lld.%09ld}\n",
                   getpid(), (unsigned long long)(pfn * 4096), pfn == first ? "true" : "false",
                   (long long)now.tv_sec, now.tv_nsec);
            fflush(stdout);
        }
        if (pfn != first) return 10;
        if (i < duration) sleep(1);
    }
    munlock(page, 4096); munmap(page, 4096); close(pm); close(fd);
    return 0;
}
