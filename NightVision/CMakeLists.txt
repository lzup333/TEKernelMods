cmake_minimum_required(VERSION 4.2)
project(NightVision)

set(CMAKE_C_STANDARD 11)

add_library(NightVision SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(NightVision
        PUBLIC
        ./mod-api
        ./tefkernel
)
