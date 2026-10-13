set(LIBBPF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libbpf")
set(LIBBPF_SRC "${LIBBPF_DIR}/src")

if(NOT EXISTS "${LIBBPF_SRC}/libbpf.c")
    message(FATAL_ERROR "libbpf sources not found in ${LIBBPF_SRC}")
endif()

add_subdirectory(libelf)

find_package(ZLIB REQUIRED)

set(LIBBPF_SOURCES
    bpf.c
    btf.c
    libbpf.c
    libbpf_utils.c
    netlink.c
    nlattr.c
    libbpf_probes.c
    bpf_prog_linfo.c
    btf_dump.c
    hashmap.c
    ringbuf.c
    strset.c
    linker.c
    gen_loader.c
    relo_core.c
    usdt.c
    zip.c
    elf.c
    features.c
    btf_iter.c
    btf_relocate.c
)
list(TRANSFORM LIBBPF_SOURCES PREPEND "${LIBBPF_SRC}/")

set(LIBBPF_PUBLIC_HEADERS
    bpf.h libbpf.h btf.h libbpf_common.h libbpf_legacy.h
    bpf_helpers.h bpf_helper_defs.h bpf_tracing.h bpf_endian.h
    bpf_core_read.h skel_internal.h libbpf_version.h usdt.bpf.h
)

set(LIBBPF_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/libbpf-include")
foreach(header IN LISTS LIBBPF_PUBLIC_HEADERS)
    configure_file("${LIBBPF_SRC}/${header}" "${LIBBPF_STAGE_DIR}/bpf/${header}" COPYONLY)
endforeach()

add_library(fas_libbpf OBJECT ${LIBBPF_SOURCES})
set_target_properties(fas_libbpf PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
)
target_include_directories(fas_libbpf
    PUBLIC
        "${LIBBPF_STAGE_DIR}"
        "${LIBBPF_DIR}/include/uapi"
    PRIVATE
        "${LIBBPF_SRC}"
        "${LIBBPF_DIR}/include"
)
target_compile_definitions(fas_libbpf
    PUBLIC LIBBPF_API=
    PRIVATE _LARGEFILE64_SOURCE _FILE_OFFSET_BITS=64
)
target_compile_options(fas_libbpf PRIVATE -w)
target_link_libraries(fas_libbpf PUBLIC fas_libelf ZLIB::ZLIB)

set(FAS_LIBBPF_INCLUDE_DIR "${LIBBPF_STAGE_DIR}" PARENT_SCOPE)
