set(FAS_BPF_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/fas.bpf.o")
if(ANDROID)
    set(BPF_ARCH_INCLUDE "${CMAKE_SYSROOT}/usr/include/${CMAKE_C_ANDROID_TOOLCHAIN_MACHINE}")
    set(BPF_SYSTEM_INCLUDE "${CMAKE_SYSROOT}/usr/include")
else()
    set(BPF_ARCH_INCLUDE "/usr/include/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu")
    set(BPF_SYSTEM_INCLUDE "/usr/include")
endif()

add_custom_command(
    OUTPUT "${FAS_BPF_OBJECT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}"
    COMMAND "${BPF_CLANG}" -O2 -g -target bpf -D__TARGET_ARCH_arm64 -Wall
            "-I${FAS_LIBBPF_INCLUDE_DIR}"
            "-I${PROJECT_SOURCE_DIR}/lib/external/libbpf/include/uapi"
            "-I${BPF_ARCH_INCLUDE}" "-I${BPF_SYSTEM_INCLUDE}" "-I${PROJECT_SOURCE_DIR}/include"
            "-I${PROJECT_SOURCE_DIR}/lib/src"
            -c "${CMAKE_CURRENT_SOURCE_DIR}/fas.bpf.c" -o "${FAS_BPF_OBJECT}"
    DEPENDS
        fas.bpf.c
        "${PROJECT_SOURCE_DIR}/include/uapi/encore_fas_uapi.h"
    VERBATIM
)

add_custom_target(fas_bpf_object DEPENDS "${FAS_BPF_OBJECT}")
set(FAS_BPF_OBJECT "${FAS_BPF_OBJECT}" PARENT_SCOPE)
