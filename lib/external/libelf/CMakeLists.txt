include(CheckSymbolExists)

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(getentropy "unistd.h" HAVE_GETENTROPY)
check_symbol_exists(mempcpy "string.h" HAVE_MEMPCPY)
check_symbol_exists(memrchr "string.h" HAVE_MEMRCHR)
check_symbol_exists(reallocarray "stdlib.h" HAVE_REALLOCARRAY)
check_symbol_exists(powerof2 "sys/param.h" HAVE_POWEROF2)
unset(CMAKE_REQUIRED_DEFINITIONS)

file(GLOB FAS_LIBELF_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_library(fas_libelf OBJECT ${FAS_LIBELF_SOURCES})
set_target_properties(fas_libelf PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
)
target_include_directories(fas_libelf
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_definitions(fas_libelf PRIVATE
    HAVE_CONFIG_H
    HAVE_DECL_GETENTROPY=$<BOOL:${HAVE_GETENTROPY}>
    HAVE_DECL_MEMPCPY=$<BOOL:${HAVE_MEMPCPY}>
    HAVE_DECL_MEMRCHR=$<BOOL:${HAVE_MEMRCHR}>
    HAVE_DECL_REALLOCARRAY=$<BOOL:${HAVE_REALLOCARRAY}>
    HAVE_DECL_POWEROF2=$<BOOL:${HAVE_POWEROF2}>
)
target_compile_options(fas_libelf PRIVATE -w)
find_package(ZLIB REQUIRED)
target_link_libraries(fas_libelf PUBLIC ZLIB::ZLIB)
