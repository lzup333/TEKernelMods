cmake_minimum_required(VERSION 4.2)
project(LightPenetration)

set(CMAKE_C_STANDARD 11)

add_library(LightPenetration SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(LightPenetration
        PUBLIC
        ./mod-api
        ./tefkernel
)
