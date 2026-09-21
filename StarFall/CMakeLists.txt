cmake_minimum_required(VERSION 4.2)
project(StarFall)

set(CMAKE_C_STANDARD 11)

add_library(StarFall SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(StarFall
        PUBLIC
        ./mod-api
        ./tefkernel
)
