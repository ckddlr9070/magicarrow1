cmake_minimum_required(VERSION 3.21)
project(SMF_MagicBullet VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# CommonLibSSE-NG is cloned into extern/ by the GitHub workflow
add_subdirectory(extern/CommonLibSSE-NG)
find_package(spdlog CONFIG REQUIRED)

add_library(${PROJECT_NAME} SHARED src/plugin.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE spdlog::spdlog)
