#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_riscv/cpu.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"

#define EI_NIDENT 16
#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_RISCV 243
#define PT_LOAD 1
#define MEM_SIZE (64u * 1024u * 1024u)
#define STACK_TOP (MEM_SIZE - 16)

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct flat_mmu {
    struct mmu mmu;
    uint8_t *mem;
    size_t size;
    addr_t brk;
};

static void rvrun_die(const char *message) {
    fprintf(stderr, "rvrun: %s\n", message);
    exit(1);
}

static void die_errno(const char *message) {
    fprintf(stderr, "rvrun: %s: %s\n", message, strerror(errno));
    exit(1);
}

static uint8_t *read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        die_errno(path);
    if (fseek(file, 0, SEEK_END) != 0)
        die_errno("fseek");
    long size = ftell(file);
    if (size < 0)
        die_errno("ftell");
    if (fseek(file, 0, SEEK_SET) != 0)
        die_errno("fseek");

    uint8_t *data = malloc((size_t) size);
    if (data == NULL)
        die_errno("malloc");
    if (fread(data, 1, (size_t) size, file) != (size_t) size)
        die_errno("fread");
    fclose(file);
    *size_out = (size_t) size;
    return data;
}

static bool range_ok(size_t file_size, uint64_t off, uint64_t size) {
    return off <= file_size && size <= file_size - off;
}

static void *flat_translate(struct mmu *mmu, addr_t addr, int type) {
    (void) type;
    struct flat_mmu *flat = (struct flat_mmu *) mmu;
    if (addr >= flat->size)
        return NULL;
    return &flat->mem[addr];
}

static void load_elf(struct flat_mmu *flat, const uint8_t *data, size_t file_size,
        struct cpu_state *cpu) {
    if (file_size < sizeof(struct elf64_ehdr))
        rvrun_die("file too small for ELF64 header");
    const struct elf64_ehdr *ehdr = (const struct elf64_ehdr *) data;
    if (memcmp(ehdr->e_ident, "\177ELF", 4) != 0)
        rvrun_die("not an ELF file");
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        rvrun_die("expected little-endian ELF64");
    if (ehdr->e_machine != EM_RISCV)
        rvrun_die("expected EM_RISCV");
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        rvrun_die("expected executable or shared-object ELF");
    if (ehdr->e_phentsize != sizeof(struct elf64_phdr))
        rvrun_die("unexpected program header size");
    if (!range_ok(file_size, ehdr->e_phoff, (uint64_t) ehdr->e_phnum * ehdr->e_phentsize))
        rvrun_die("program header table extends past end of file");

    const struct elf64_phdr *phdrs = (const struct elf64_phdr *) (data + ehdr->e_phoff);
    addr_t high = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;
        if (!range_ok(file_size, ph->p_offset, ph->p_filesz))
            rvrun_die("load segment extends past end of file");
        if (ph->p_vaddr >= flat->size || ph->p_memsz > flat->size - ph->p_vaddr)
            rvrun_die("load segment does not fit in flat test memory");
        if (ph->p_filesz > ph->p_memsz)
            rvrun_die("load segment filesz exceeds memsz");
        memcpy(flat->mem + ph->p_vaddr, data + ph->p_offset, ph->p_filesz);
        memset(flat->mem + ph->p_vaddr + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
        if (ph->p_vaddr + ph->p_memsz > high)
            high = ph->p_vaddr + ph->p_memsz;
    }

    flat->brk = (high + PAGE_SIZE - 1) & ~(addr_t) (PAGE_SIZE - 1);
    cpu->pc = ehdr->e_entry;
    cpu->sp = STACK_TOP;
}

static int handle_syscall(struct flat_mmu *flat, struct cpu_state *cpu) {
    switch (cpu->a7) {
    case 64: { // write
        int fd = (int) cpu->a0;
        addr_t buf = cpu->a1;
        size_t len = cpu->a2;
        if (buf >= flat->size || len > flat->size - buf) {
            cpu->a0 = (uint64_t) -EFAULT;
            return 0;
        }
        FILE *out = fd == 2 ? stderr : stdout;
        if (fd != 1 && fd != 2) {
            cpu->a0 = (uint64_t) -EBADF;
            return 0;
        }
        cpu->a0 = fwrite(flat->mem + buf, 1, len, out);
        fflush(out);
        return 0;
    }
    case 93: // exit
    case 94: // exit_group
        return (int) cpu->a0;
    case 214: // brk
        if (cpu->a0 != 0 && cpu->a0 < flat->size)
            flat->brk = cpu->a0;
        cpu->a0 = flat->brk;
        return 0;
    default:
        fprintf(stderr, "rvrun: unsupported syscall %" PRIu64 "\n", cpu->a7);
        cpu->a0 = (uint64_t) -ENOSYS;
        return 0;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }

    size_t file_size;
    uint8_t *file = read_file(argv[1], &file_size);
    static struct mmu_ops ops = {
        .translate = flat_translate,
    };
    struct flat_mmu flat = {
        .mmu = {
            .ops = &ops,
            .changes = 1,
        },
        .mem = calloc(1, MEM_SIZE),
        .size = MEM_SIZE,
    };
    if (flat.mem == NULL)
        die_errno("calloc");

    struct cpu_state cpu = {
        .mmu = &flat.mmu,
    };
    load_elf(&flat, file, file_size, &cpu);

    struct tlb tlb = {};
    tlb_refresh(&tlb, &flat.mmu);

    for (;;) {
        int interrupt = cpu_run_to_interrupt(&cpu, &tlb);
        if (interrupt == INT_SYSCALL) {
            int exit_code = handle_syscall(&flat, &cpu);
            if (cpu.a7 == 93 || cpu.a7 == 94)
                return exit_code;
        } else {
            fprintf(stderr, "rvrun: interrupt %d at pc=0x%" PRIx64 "\n", interrupt, cpu.pc);
            return 128 + interrupt;
        }
    }
}
