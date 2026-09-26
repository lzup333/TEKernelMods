cmake_minimum_required(VERSION 4.2)
project(Vampire)

set(CMAKE_C_STANDARD 11)

add_library(Vampire SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(Vampire
        PUBLIC
        ./mod-api
        ./tefkernel
)
