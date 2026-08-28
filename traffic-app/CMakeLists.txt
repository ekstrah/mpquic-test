cmake_minimum_required(VERSION 3.5)
project(mp_traffic C)

set(CMAKE_C_STANDARD 99)

# Reuse the sibling picoquic checkout's own CMake project rather than
# re-deriving its OpenSSL/picotls dependency resolution by hand - this
# is the standard CMake pattern for depending on a sibling project.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../picoquic ${CMAKE_CURRENT_BINARY_DIR}/picoquic-build)

add_executable(mp_traffic mp_traffic.c pacer.c)
target_include_directories(mp_traffic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../picoquic/picoquic)
# picoquic-core and picoquic-log reference each other's symbols
# (sockloop.c in picoquic-core calls picoquic_set_qlog from picoquic-log);
# GNU ld only makes one left-to-right pass over static libraries, so
# picoquic-log is listed twice to give it a second pass once picoquic-core
# has created the need for its symbol.
target_link_libraries(mp_traffic PRIVATE picoquic-log picoquic-core picoquic-log ${PTLS_LIBRARIES} ${OPENSSL_LIBRARIES})

add_executable(test_pacer test_pacer.c pacer.c)
