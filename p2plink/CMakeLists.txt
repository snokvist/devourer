cmake_minimum_required(VERSION 3.20)
project(p2plink CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The sibling devourer tree is the radio library. Chip set: the fleet dies this
# link flies on. Everything else off keeps the binary small and the failure
# surface narrow (an 8812EU drone, 8812EU/MT7612U ground).
option(P2P_WITH_MT7612U "Build the MediaTek MT7612U backend (ground ears)" ON)
set(DEVOURER_JAGUAR1        OFF CACHE BOOL "" FORCE)
set(DEVOURER_8814           OFF CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR2_8822B  OFF CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR2_8821C  OFF CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR3_8822C  ON  CACHE BOOL "" FORCE)
set(DEVOURER_JAGUAR3_8822E  ON  CACHE BOOL "" FORCE)
set(DEVOURER_8733B          OFF CACHE BOOL "" FORCE)
set(DEVOURER_KESTREL_8852B  OFF CACHE BOOL "" FORCE)
set(DEVOURER_KESTREL_8852C  OFF CACHE BOOL "" FORCE)
set(DEVOURER_MT7612U ${P2P_WITH_MT7612U} CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_BINARY_DIR}/devourer EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Pure core: no devourer, no sockets, no threads. Header-only + fec.cpp.
add_library(p2p_core STATIC src/fec.cpp)
target_include_directories(p2p_core PUBLIC include)

# Device-facing layer: devourer glue, tun, frame ring.
add_library(p2p_io STATIC src/radio.cpp src/tun.cpp src/frame_shm.cpp src/stats_pub.cpp)
target_include_directories(p2p_io PUBLIC include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/common)
target_link_libraries(p2p_io PUBLIC p2p_core devourer PkgConfig::libusb Threads::Threads rt)

add_executable(p2p_tx src/p2p_tx.cpp)
target_link_libraries(p2p_tx PRIVATE p2p_io)
add_executable(p2p_rx src/p2p_rx.cpp)
target_link_libraries(p2p_rx PRIVATE p2p_io)
add_executable(p2p_feed src/p2p_feed.cpp)
target_link_libraries(p2p_feed PRIVATE p2p_io)

enable_testing()
foreach(t wire fec merge link_ctl budget)
  add_executable(${t}_test tests/${t}_test.cpp)
  target_link_libraries(${t}_test PRIVATE p2p_core)
  add_test(NAME ${t} COMMAND ${t}_test)
  set_tests_properties(${t} PROPERTIES LABELS p2p)
endforeach()
# Device-free but Linux-only (shm/futex/eventfd): links the io layer without a radio.
add_executable(frame_shm_test tests/frame_shm_test.cpp src/frame_shm.cpp)
target_include_directories(frame_shm_test PRIVATE include)
target_link_libraries(frame_shm_test PRIVATE Threads::Threads rt)
add_test(NAME frame_shm COMMAND frame_shm_test)
set_tests_properties(frame_shm PROPERTIES LABELS p2p)
# `ctest -L p2p`: the devourer subtree is EXCLUDE_FROM_ALL, so its own
# selftests are registered but never built here and would read "Not Run".
