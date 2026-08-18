cmake_minimum_required(VERSION 4.2)
project(RandomCraft)

set(CMAKE_C_STANDARD 11)

add_library(RandomCraft SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(RandomCraft
        PUBLIC
        ./mod-api
        ./tefkernel
)
