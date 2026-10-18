cmake_minimum_required(VERSION 3.25)
project(databento_to_lobster LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fmt CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(zstd CONFIG REQUIRED)

set(DATABENTO_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)  # harmless if the option doesn't exist
add_subdirectory(external/databento-cpp)

add_executable(databento_to_lobster
  src/main.cpp
  src/book.cpp
  src/message.cpp
  src/book_constructor.cpp
)
target_include_directories(databento_to_lobster PRIVATE include)
target_link_libraries(databento_to_lobster PRIVATE databento::databento fmt::fmt)
target_compile_options(databento_to_lobster PRIVATE /W4 /utf-8 /permissive-)