add_executable(fas_tail fas_tail.c)
target_link_libraries(fas_tail PRIVATE fas_static)
target_compile_options(fas_tail PRIVATE -Wall -Wextra)
set_target_properties(fas_tail PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS fas_tail RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
