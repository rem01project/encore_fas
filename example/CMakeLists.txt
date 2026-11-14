add_executable(fas_tail fas_tail.cpp fas_sched.cpp fas_sysfs.cpp)
target_link_libraries(fas_tail PRIVATE fas_static)
target_compile_options(fas_tail PRIVATE -Wall -Wextra)
set_target_properties(fas_tail PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(fas_sched_sim fas_sched_sim.cpp fas_sched.cpp)
target_include_directories(fas_sched_sim PRIVATE
    ${PROJECT_SOURCE_DIR}/lib/include
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(fas_sched_sim PRIVATE -Wall -Wextra)
set_target_properties(fas_sched_sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS fas_tail RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
