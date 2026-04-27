#include "asbestos/profile.h"

#if ASBESTOS_INSTRUMENT
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define PROFILE_OPCODE_BUCKETS 512
#define PROFILE_INTERRUPT_BUCKETS 256
#define PROFILE_SYSCALL_BUCKETS 512

struct profile_bucket {
    atomic_uint_fast64_t count;
    atomic_uint_fast64_t code_words;
    atomic_uint_fast64_t guest_bytes;
};

static struct profile_bucket opcodes[PROFILE_OPCODE_BUCKETS];
static struct profile_bucket blocks;
static atomic_uint_fast64_t interrupts[PROFILE_INTERRUPT_BUCKETS];
static atomic_uint_fast64_t syscalls[PROFILE_SYSCALL_BUCKETS];
static pthread_once_t profile_once = PTHREAD_ONCE_INIT;
static bool profile_enabled;

static void profile_dump(void) {
    const char *path = getenv("ISH_ASBESTOS_PROFILE");
    FILE *out = stdout;

    if (!profile_enabled)
        return;
    if (path != NULL && path[0] != '\0') {
        out = fopen(path, "w");
        if (out == NULL)
            out = stdout;
    }

    fprintf(out, "kind\tkey\tcount\tcode_words\tguest_bytes\n");
    for (unsigned i = 0; i < PROFILE_OPCODE_BUCKETS; i++) {
        uint64_t count = atomic_load_explicit(&opcodes[i].count, memory_order_relaxed);
        if (count == 0)
            continue;
        fprintf(out, "opcode\t%03x\t%llu\t%llu\t%llu\n", i,
                (unsigned long long) count,
                (unsigned long long) atomic_load_explicit(&opcodes[i].code_words, memory_order_relaxed),
                (unsigned long long) atomic_load_explicit(&opcodes[i].guest_bytes, memory_order_relaxed));
    }
    fprintf(out, "blocks\tall\t%llu\t%llu\t%llu\n",
            (unsigned long long) atomic_load_explicit(&blocks.count, memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&blocks.code_words, memory_order_relaxed),
            (unsigned long long) atomic_load_explicit(&blocks.guest_bytes, memory_order_relaxed));
    for (unsigned i = 0; i < PROFILE_INTERRUPT_BUCKETS; i++) {
        uint64_t count = atomic_load_explicit(&interrupts[i], memory_order_relaxed);
        if (count != 0)
            fprintf(out, "interrupt\t%u\t%llu\t0\t0\n", i, (unsigned long long) count);
    }
    for (unsigned i = 0; i < PROFILE_SYSCALL_BUCKETS; i++) {
        uint64_t count = atomic_load_explicit(&syscalls[i], memory_order_relaxed);
        if (count != 0)
            fprintf(out, "syscall\t%u\t%llu\t0\t0\n", i, (unsigned long long) count);
    }

    if (out != stdout)
        fclose(out);
}

static void profile_init(void) {
    const char *path = getenv("ISH_ASBESTOS_PROFILE");
    profile_enabled = path != NULL;
    if (profile_enabled)
        atexit(profile_dump);
}

bool asbestos_profile_is_enabled(void) {
    pthread_once(&profile_once, profile_init);
    return profile_enabled;
}

void asbestos_profile_record_opcode(unsigned key, uint64_t code_words, uint64_t guest_bytes) {
    if (!asbestos_profile_is_enabled())
        return;
    if (key >= PROFILE_OPCODE_BUCKETS)
        key = PROFILE_OPCODE_BUCKETS - 1;
    atomic_fetch_add_explicit(&opcodes[key].count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&opcodes[key].code_words, code_words, memory_order_relaxed);
    atomic_fetch_add_explicit(&opcodes[key].guest_bytes, guest_bytes, memory_order_relaxed);
}

void asbestos_profile_record_block(uint64_t code_words, uint64_t guest_bytes) {
    if (!asbestos_profile_is_enabled())
        return;
    atomic_fetch_add_explicit(&blocks.count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&blocks.code_words, code_words, memory_order_relaxed);
    atomic_fetch_add_explicit(&blocks.guest_bytes, guest_bytes, memory_order_relaxed);
}

void asbestos_profile_record_interrupt(int interrupt) {
    if (!asbestos_profile_is_enabled())
        return;
    if (interrupt < 0 || interrupt >= PROFILE_INTERRUPT_BUCKETS)
        interrupt = PROFILE_INTERRUPT_BUCKETS - 1;
    atomic_fetch_add_explicit(&interrupts[interrupt], 1, memory_order_relaxed);
}

void asbestos_profile_record_syscall(unsigned syscall) {
    if (!asbestos_profile_is_enabled())
        return;
    if (syscall >= PROFILE_SYSCALL_BUCKETS)
        syscall = PROFILE_SYSCALL_BUCKETS - 1;
    atomic_fetch_add_explicit(&syscalls[syscall], 1, memory_order_relaxed);
}

#else

bool asbestos_profile_is_enabled(void) {
    return false;
}

void asbestos_profile_record_opcode(unsigned key, uint64_t code_words, uint64_t guest_bytes) {
    (void) key;
    (void) code_words;
    (void) guest_bytes;
}

void asbestos_profile_record_block(uint64_t code_words, uint64_t guest_bytes) {
    (void) code_words;
    (void) guest_bytes;
}

void asbestos_profile_record_interrupt(int interrupt) {
    (void) interrupt;
}

void asbestos_profile_record_syscall(unsigned syscall) {
    (void) syscall;
}

#endif
