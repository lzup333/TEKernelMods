cmake_minimum_required(VERSION 4.2)
project(ChaosGen)

set(CMAKE_C_STANDARD 11)

add_library(ChaosGen SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(ChaosGen
        PUBLIC
        ./mod-api
        ./tefkernel
)
