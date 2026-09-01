cmake_minimum_required(VERSION 4.2)
project(InfiniteBackpack)

set(CMAKE_C_STANDARD 11)

add_library(InfiniteBackpack SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(InfiniteBackpack
        PUBLIC
        ./mod-api
        ./tefkernel
)
