cmake_minimum_required(VERSION 3.25)
project(homenvr-backend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- Зависимости (apt-пакеты Debian 13, см. Dockerfile) ---
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Библиотеки без CMake-config в apt — ищем вручную / через pkg-config
find_library(PQXX_LIB pqxx REQUIRED)
find_library(ARGON2_LIB argon2 REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL   REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(PUGIXML  REQUIRED IMPORTED_TARGET pugixml)

# Crow — header-only, через FetchContent (нужен standalone asio из apt)
find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)

include(FetchContent)
FetchContent_Declare(
    crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.0
    GIT_SHALLOW TRUE
)
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CROW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(crow)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(homenvr-backend ${SOURCES})

target_include_directories(homenvr-backend PRIVATE
    ${ASIO_INCLUDE_DIR}
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(homenvr-backend PRIVATE
    Crow::Crow
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::PUGIXML
    ${PQXX_LIB}
    ${ARGON2_LIB}
    ${PostgreSQL_LIBRARIES}
    Threads::Threads
)
