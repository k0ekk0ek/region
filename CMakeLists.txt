cmake_minimum_required(VERSION 3.10)
project(region VERSION 0.0.1 LANGUAGES C)

add_executable(alloc src/region.c src/alloc.c)
