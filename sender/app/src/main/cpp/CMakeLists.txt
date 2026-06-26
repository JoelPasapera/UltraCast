cmake_minimum_required(VERSION 3.22.1)
project(udpsender)

# Ahora incluye el códec FEC compartido.
add_library(udpsender SHARED udp_sender.cpp fec.cpp)

find_library(log-lib log)
target_link_libraries(udpsender ${log-lib})
