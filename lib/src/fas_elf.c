// SPDX-License-Identifier: BSD-2-Clause

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fas/fas.h"

#ifndef FAS_ELF_MACHINE
    #if defined(__aarch64__)
        #define FAS_ELF_MACHINE EM_AARCH64
    #elif defined(__x86_64__)
        #define FAS_ELF_MACHINE EM_X86_64
    #else
        #error "Define FAS_ELF_MACHINE for this machine."
    #endif
#endif

struct elf_view {
    const uint8_t *base;
    size_t size;
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *sections;
};

static int in_file(const struct elf_view *v, uint64_t off, uint64_t len) {
    return off <= v->size && len <= v->size - off;
}

static int vaddr_to_offset(const struct elf_view *v, uint64_t vaddr, uint64_t *out) {
    const Elf64_Phdr *ph;

    if (v->eh->e_phentsize != sizeof(Elf64_Phdr) || !in_file(v, v->eh->e_phoff, (uint64_t)v->eh->e_phnum * sizeof(*ph)))
        return -ENOENT;

    ph = (const Elf64_Phdr *)(v->base + v->eh->e_phoff);
    for (int i = 0; i < v->eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !in_file(v, ph[i].p_offset, ph[i].p_filesz))
            continue;
        if (vaddr >= ph[i].p_vaddr && vaddr - ph[i].p_vaddr < ph[i].p_filesz) {
            *out = ph[i].p_offset + (vaddr - ph[i].p_vaddr);
            return 0;
        }
    }
    return -ENOENT;
}

static int find_in_table(const struct elf_view *v, const Elf64_Shdr *symtab, const char *name, uint64_t *vaddr) {
    const Elf64_Shdr *strtab;
    const Elf64_Sym *sym;
    const char *str;
    size_t want = strlen(name);
    size_t count;

    if (symtab->sh_link >= v->eh->e_shnum || symtab->sh_entsize != sizeof(Elf64_Sym))
        return -ENOENT;
    strtab = &v->sections[symtab->sh_link];
    if (!in_file(v, symtab->sh_offset, symtab->sh_size) || !in_file(v, strtab->sh_offset, strtab->sh_size) || strtab->sh_size == 0)
        return -ENOENT;

    sym = (const Elf64_Sym *)(v->base + symtab->sh_offset);
    str = (const char *)(v->base + strtab->sh_offset);
    count = symtab->sh_size / sizeof(*sym);

    for (size_t i = 0; i < count; i++) {
        if (ELF64_ST_TYPE(sym[i].st_info) != STT_FUNC || sym[i].st_shndx == SHN_UNDEF || sym[i].st_value == 0)
            continue;
        if (sym[i].st_name >= strtab->sh_size || strtab->sh_size - sym[i].st_name <= want)
            continue;
        if (memcmp(str + sym[i].st_name, name, want + 1) == 0) {
            *vaddr = sym[i].st_value;
            return 0;
        }
    }
    return -ENOENT;
}

int fas_resolve_offset(const char *lib_path, const char *const *symbols, size_t count, uint64_t *out_offset) {
    struct elf_view v = {0};
    struct stat st;
    int rc = -ENOENT;
    int fd;

    if (!lib_path || !symbols || !out_offset)
        return -EINVAL;

    fd = open(lib_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd);
        return -EINVAL;
    }

    v.size = (size_t)st.st_size;
    v.base = mmap(NULL, v.size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (v.base == MAP_FAILED)
        return -errno;

    v.eh = (const Elf64_Ehdr *)v.base;
    if (memcmp(v.eh->e_ident, ELFMAG, SELFMAG) != 0 || v.eh->e_ident[EI_CLASS] != ELFCLASS64 || v.eh->e_ident[EI_DATA] != ELFDATA2LSB || v.eh->e_machine != FAS_ELF_MACHINE || v.eh->e_shentsize != sizeof(Elf64_Shdr) ||
        !in_file(&v, v.eh->e_shoff, (uint64_t)v.eh->e_shnum * sizeof(Elf64_Shdr))) {
        rc = -EINVAL;
        goto out;
    }
    v.sections = (const Elf64_Shdr *)(v.base + v.eh->e_shoff);

    for (size_t c = 0; c < count && rc != 0; c++) {
        for (int i = 0; i < v.eh->e_shnum && rc != 0; i++) {
            uint64_t vaddr;

            if (v.sections[i].sh_type != SHT_SYMTAB && v.sections[i].sh_type != SHT_DYNSYM)
                continue;
            if (find_in_table(&v, &v.sections[i], symbols[c], &vaddr) == 0)
                rc = vaddr_to_offset(&v, vaddr, out_offset);
        }
    }

out:
    munmap((void *)v.base, v.size);
    return rc;
}
