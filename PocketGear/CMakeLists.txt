cmake_minimum_required(VERSION 4.2)
project(PocketGear)

set(CMAKE_C_STANDARD 11)

add_library(PocketGear SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(PocketGear
        PUBLIC
        ./mod-api
        ./tefkernel
)
