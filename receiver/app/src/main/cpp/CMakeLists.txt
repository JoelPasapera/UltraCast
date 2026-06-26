cmake_minimum_required(VERSION 3.22.1)
project(udpreceiver)

# Ahora incluye el códec FEC compartido.
add_library(udpreceiver SHARED udp_receiver.cpp fec.cpp)

find_library(log-lib log)
target_link_libraries(udpreceiver ${log-lib})

