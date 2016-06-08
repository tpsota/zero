###############################################
###              Benchmark                  ###
###############################################

cmake_minimum_required(VERSION 2.8.4)
project(Benchmark)

message(STATUS "${PROJECT_NAME} settings:")
message(STATUS "==============\n")

###''' SOURCE AND HEADER FILES '''###

file(GLOB_RECURSE SOURCE_FILES *.cpp)
file(GLOB_RECURSE HEADER_FILES *.h)

message(STATUS "Source Files:")
message(STATUS "${SOURCE_FILES} \n")
message(STATUS "Header Files:")
message(STATUS "${HEADER_FILES} \n")

### BenchmarkLib ###

### Benchmark ###

add_executable(Benchmark ${SOURCE_FILES} ${HEADER_FILES})
target_include_directories(Benchmark PRIVATE ${Boost_INCLUDE_DIR})
target_link_libraries(Benchmark zapps_base loginspect kits restore sm)
