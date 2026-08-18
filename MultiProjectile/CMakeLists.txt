cmake_minimum_required(VERSION 4.2)
project(MultiProjectile)

set(CMAKE_C_STANDARD 11)

add_library(MultiProjectile SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(MultiProjectile
        PUBLIC
        ./mod-api
        ./tefkernel
)
