cmake_minimum_required(VERSION 4.2)
project(main)

set(CMAKE_C_STANDARD  11)

add_library(main SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(main
        PUBLIC
        ./mod-api
        ./tefkernel
)
