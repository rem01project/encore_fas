// SPDX-License-Identifier: GPL-2.0-only

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    void *data;
    size_t size;
    Elf64_Ehdr *ehdr;
    Elf64_Shdr *shdr;
    char *shstrtab;
} elf_file_t;

static int open_elf_file(const char *path, elf_file_t *elf) {
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", path);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "Error: Cannot stat file '%s'\n", path);
        close(fd);
        return -1;
    }

    elf->size = st.st_size;
    elf->data = mmap(NULL, elf->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (elf->data == MAP_FAILED) {
        fprintf(stderr, "Error: Cannot mmap file '%s'\n", path);
        return -1;
    }

    elf->ehdr = (Elf64_Ehdr *)elf->data;

    if (memcmp(elf->ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "Error: '%s' is not a valid ELF file\n", path);
        munmap(elf->data, elf->size);
        return -1;
    }

    if (elf->ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "Error: '%s' is not a 64-bit ELF file\n", path);
        munmap(elf->data, elf->size);
        return -1;
    }

    elf->shdr = (Elf64_Shdr *)((char *)elf->data + elf->ehdr->e_shoff);
    elf->shstrtab = (char *)elf->data + elf->shdr[elf->ehdr->e_shstrndx].sh_offset;

    return 0;
}

static void close_elf_file(elf_file_t *elf) {
    if (elf && elf->data && elf->data != MAP_FAILED)
        munmap(elf->data, elf->size);
}

static Elf64_Shdr *find_symtab_section(elf_file_t *elf) {
    for (int i = 0; i < elf->ehdr->e_shnum; i++) {
        if (elf->shdr[i].sh_type == SHT_SYMTAB)
            return &elf->shdr[i];
    }
    return NULL;
}

static Elf64_Sym *find_symbol_by_name(elf_file_t *elf, const char *name, Elf64_Shdr *symtab, const char *strtab) {
    Elf64_Sym *syms = (Elf64_Sym *)((char *)elf->data + symtab->sh_offset);
    int sym_count = symtab->sh_size / sizeof(Elf64_Sym);

    for (int i = 0; i < sym_count; i++) {
        const char *sym_name = strtab + syms[i].st_name;
        if (strcmp(sym_name, name) == 0)
            return &syms[i];
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <kernel_module.ko> <vmlinux>\n", argv[0]);
        return 1;
    }

    const char *ko_path = argv[1];
    const char *vmlinux_path = argv[2];

    elf_file_t ko_elf, vmlinux_elf;

    if (open_elf_file(ko_path, &ko_elf) < 0)
        return 1;

    if (open_elf_file(vmlinux_path, &vmlinux_elf) < 0) {
        close_elf_file(&ko_elf);
        return 1;
    }

    Elf64_Shdr *ko_symtab = find_symtab_section(&ko_elf);
    Elf64_Shdr *vmlinux_symtab = find_symtab_section(&vmlinux_elf);

    if (!ko_symtab) {
        fprintf(stderr, "Error: No symbol table found in '%s'\n", ko_path);
        close_elf_file(&ko_elf);
        close_elf_file(&vmlinux_elf);
        return 1;
    }

    if (!vmlinux_symtab) {
        fprintf(stderr, "Error: No symbol table found in '%s'\n", vmlinux_path);
        close_elf_file(&ko_elf);
        close_elf_file(&vmlinux_elf);
        return 1;
    }

    const char *ko_strtab = (char *)ko_elf.data + ko_elf.shdr[ko_symtab->sh_link].sh_offset;
    const char *vmlinux_strtab = (char *)vmlinux_elf.data + vmlinux_elf.shdr[vmlinux_symtab->sh_link].sh_offset;

    Elf64_Sym *ko_syms = (Elf64_Sym *)((char *)ko_elf.data + ko_symtab->sh_offset);
    int ko_sym_count = ko_symtab->sh_size / sizeof(Elf64_Sym);

    int has_error = 0;

    for (int i = 0; i < ko_sym_count; i++) {
        if (ko_syms[i].st_shndx == SHN_UNDEF && ko_syms[i].st_name != 0) {
            const char *sym_name = ko_strtab + ko_syms[i].st_name;

            Elf64_Sym *vmlinux_sym = find_symbol_by_name(&vmlinux_elf, sym_name, vmlinux_symtab, vmlinux_strtab);

            if (!vmlinux_sym || vmlinux_sym->st_shndx == SHN_UNDEF) {
                fprintf(stderr, "Error: Symbol '%s' not found or undefined in %s\n", sym_name, vmlinux_path);
                has_error = 1;
            } else {
                int binding = ELF64_ST_BIND(vmlinux_sym->st_info);

                if (binding != STB_GLOBAL && binding != STB_WEAK) {
                    fprintf(stderr,
                            "Warning: Symbol '%s' is defined in %s but not global (binding=%d)\n",
                            sym_name,
                            vmlinux_path,
                            binding);
                }
            }
        }
    }

    close_elf_file(&ko_elf);
    close_elf_file(&vmlinux_elf);

    return has_error ? 1 : 0;
}
