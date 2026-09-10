cmake_minimum_required(VERSION 4.2)
project(愿者上钩)

set(CMAKE_C_STANDARD 11)

add_library(VoluntaryHook SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(VoluntaryHook
        PUBLIC
        ./mod-api
        ./tefkernel
)
