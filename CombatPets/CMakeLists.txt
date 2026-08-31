cmake_minimum_required(VERSION 4.2)
project(CombatPets)

set(CMAKE_C_STANDARD 11)

add_library(CombatPets SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_link_directories(CombatPets
        PUBLIC
        ./mod-api
        ./tefkernel
)
