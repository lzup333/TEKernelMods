cmake_minimum_required(VERSION 4.2)
project(ManaLock)

set(CMAKE_C_STANDARD 11)

add_library(ManaLock SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(ManaLock
        PUBLIC
        ./mod-api
        ./tefkernel
)
