cmake_minimum_required(VERSION 4.2)
project(RandomLoot)

set(CMAKE_C_STANDARD 11)

add_library(RandomLoot SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(RandomLoot
        PUBLIC
        ./mod-api
        ./tefkernel
)
