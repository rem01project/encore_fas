set(FAS_SOURCES
    src/fas.c
    src/fas_elf.c
    src/fas_frame.c
    src/be_lkm.c
    src/be_replay.c
    src/be_bpf.c
    src/ringbuf.c
)

if(FAS_WITH_BPF)
    add_subdirectory(external)
    add_subdirectory(bpf)
    list(APPEND FAS_SOURCES src/bpf_embed.c)
endif()

function(configure_fas_library target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden
    )
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(FAS_WITH_BPF)
        add_dependencies(${target} fas_bpf_object)
        target_compile_definitions(${target} PRIVATE
            FAS_WITH_BPF
            "FAS_BPF_OBJ_PATH=\"${FAS_BPF_OBJECT}\""
        )
        target_link_libraries(${target} PRIVATE fas_libbpf fas_libelf)
    endif()
endfunction()

add_library(fas_static STATIC ${FAS_SOURCES})
set_target_properties(fas_static PROPERTIES OUTPUT_NAME fas)
configure_fas_library(fas_static)

add_library(fas_shared SHARED ${FAS_SOURCES})
set_target_properties(fas_shared PROPERTIES OUTPUT_NAME fas)
configure_fas_library(fas_shared)

set_target_properties(fas_static fas_shared PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

install(TARGETS fas_static fas_shared
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/fas DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${PROJECT_SOURCE_DIR}/include/uapi/encore_fas_uapi.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uapi)
