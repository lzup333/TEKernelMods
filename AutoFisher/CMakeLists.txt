cmake_minimum_required(VERSION 4.2)
project(AutoFisher)

set(CMAKE_C_STANDARD 11)

add_library(AutoFisher SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(AutoFisher
        PUBLIC
        ./mod-api
        ./tefkernel
)
