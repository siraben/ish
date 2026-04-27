#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE_SIZE_ 4096
#define PAGES 64
#define THREADS 8
#define LOOPS 200

static volatile unsigned char *area;

static void *worker(void *arg) {
    long id = (long) arg;

    for (int loop = 0; loop < LOOPS; loop++) {
        for (int page = 0; page < PAGES; page++)
            area[page * PAGE_SIZE_] ^= (unsigned char) (id + loop + page);
    }

    return NULL;
}

int main(void) {
    area = mmap(NULL, PAGES * PAGE_SIZE_, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset((void *) area, 0x5a, PAGES * PAGE_SIZE_);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        pthread_t threads[THREADS];
        for (long i = 0; i < THREADS; i++) {
            if (pthread_create(&threads[i], NULL, worker, (void *) i) != 0)
                _exit(2);
        }
        for (int i = 0; i < THREADS; i++) {
            if (pthread_join(threads[i], NULL) != 0)
                _exit(3);
        }
        write(STDOUT_FILENO, "cow ok\n", 7);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child failed: status=%d\n", status);
        return 1;
    }

    return 0;
}
