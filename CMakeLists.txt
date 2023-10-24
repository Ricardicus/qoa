cmake_minimum_required(VERSION 3.10)

project(QOA)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add library target
add_library(QOA qoa.cpp qoa.h)
target_include_directories(QOA PUBLIC .)
