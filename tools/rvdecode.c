#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_riscv/decode.h"

#define EI_NIDENT 16
#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_RISCV 243
#define SHT_PROGBITS 1
#define SHF_EXECINSTR 0x4

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

struct elf64_shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

static void die(const char *message) {
    fprintf(stderr, "rvdecode: %s\n", message);
    exit(1);
}

static void die_errno(const char *message) {
    fprintf(stderr, "rvdecode: %s: %s\n", message, strerror(errno));
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

static const char *section_name(const uint8_t *data, size_t file_size,
        const struct elf64_ehdr *ehdr, const struct elf64_shdr *shdrs,
        const struct elf64_shdr *shdr) {
    if (ehdr->e_shstrndx >= ehdr->e_shnum)
        return "";
    const struct elf64_shdr *strings = &shdrs[ehdr->e_shstrndx];
    if (!range_ok(file_size, strings->sh_offset, strings->sh_size))
        return "";
    if (shdr->sh_name >= strings->sh_size)
        return "";
    return (const char *) data + strings->sh_offset + shdr->sh_name;
}

static void decode_section(const uint8_t *data, size_t file_size,
        const struct elf64_shdr *shdr, const char *name) {
    if (!range_ok(file_size, shdr->sh_offset, shdr->sh_size))
        die("section extends past end of file");

    const uint8_t *code = data + shdr->sh_offset;
    size_t remaining = shdr->sh_size;
    uint64_t pc = shdr->sh_addr;

    printf("\n%s:\n", name);
    while (remaining > 0) {
        struct rv_insn insn;
        bool ok = rv_decode(code, remaining, &insn);
        unsigned length = insn.length ? insn.length : 2;
        printf("%016" PRIx64 ":  ", pc);
        if (length == 2)
            printf("%04x      ", insn.raw & 0xffff);
        else
            printf("%08x  ", insn.raw);

        if (ok && insn.operands[0] != '\0')
            printf("%-8s %s", insn.mnemonic, insn.operands);
        else
            printf("%-8s %s", insn.mnemonic, insn.operands);
        if (insn.compressed)
            printf("  # expands to 0x%08x", insn.expanded);
        putchar('\n');

        if (length > remaining)
            break;
        code += length;
        remaining -= length;
        pc += length;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }

    size_t file_size;
    uint8_t *data = read_file(argv[1], &file_size);
    if (file_size < sizeof(struct elf64_ehdr))
        die("file is too small for an ELF64 header");

    const struct elf64_ehdr *ehdr = (const struct elf64_ehdr *) data;
    if (memcmp(ehdr->e_ident, "\177ELF", 4) != 0)
        die("not an ELF file");
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        die("expected little-endian ELF64");
    if (ehdr->e_machine != EM_RISCV)
        die("expected EM_RISCV");
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        die("expected executable or shared-object ELF");
    if (ehdr->e_shentsize != sizeof(struct elf64_shdr))
        die("unexpected section header size");
    if (!range_ok(file_size, ehdr->e_shoff, (uint64_t) ehdr->e_shnum * ehdr->e_shentsize))
        die("section table extends past end of file");

    const struct elf64_shdr *shdrs = (const struct elf64_shdr *) (data + ehdr->e_shoff);
    bool decoded_any = false;
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        const struct elf64_shdr *shdr = &shdrs[i];
        const char *name = section_name(data, file_size, ehdr, shdrs, shdr);
        if (strcmp(name, ".text") != 0 &&
                !(shdr->sh_type == SHT_PROGBITS && (shdr->sh_flags & SHF_EXECINSTR)))
            continue;
        decode_section(data, file_size, shdr, name[0] ? name : "<exec>");
        decoded_any = true;
    }

    free(data);
    if (!decoded_any)
        die("no executable sections found");
    return 0;
}
