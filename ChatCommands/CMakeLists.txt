cmake_minimum_required(VERSION 4.2)
project(ChatCommands)

set(CMAKE_C_STANDARD 11)

add_library(ChatCommands SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(ChatCommands
        PUBLIC
        ./mod-api
        ./tefkernel
)
