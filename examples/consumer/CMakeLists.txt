cmake_minimum_required(VERSION 3.16)
project(hotkv_consumer C)

# Build against an installed libhotkv:
#   cmake -S examples/consumer -B build-consumer -DCMAKE_PREFIX_PATH=/path/to/install
find_package(hotkv 1 REQUIRED)

add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE hotkv::hotkv)
