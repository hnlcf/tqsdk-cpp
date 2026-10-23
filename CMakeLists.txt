cmake_minimum_required(VERSION 3.20)
project(tqsdk_cpp LANGUAGES CXX)

# ---------- 全局设置 ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------- vcpkg 依赖查找 ----------
find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
find_package(OpenSSL CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# GoogleTest (仅在启用测试时)
option(TQSDK_BUILD_TESTS "Build tests" OFF)
if(TQSDK_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# ---------- 核心库 (不依赖 CTP) ----------
add_library(tqsdk_core
    src/api/tq_api.cpp
    # src/data/data_manager.cpp
    # src/data/diff_merge_engine.cpp
    # src/channel/channel.cpp
    # src/gateway/tq_md_gateway.cpp
    # src/gateway/tq_td_gateway.cpp
    # src/gateway/sim_md_gateway.cpp
    # src/gateway/sim_td_gateway.cpp
)

target_precompile_headers(tqsdk_core PRIVATE include/tqsdk/pch.h)

target_include_directories(tqsdk_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(tqsdk_core PUBLIC
    Boost::asio
    Boost::beast
    OpenSSL::SSL
    OpenSSL::Crypto
    magic_enum::magic_enum
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# ---------- CTP 网关 (可选，仅当找到 CTP SDK 时编译) ----------
# 将 CTP 的 .so/.dll 和头文件放在 third_party/ctp/ 下：
#   third_party/ctp/include/   (ThostFtdc*.h)
#   third_party/ctp/lib/       (libthosttraderapi_se.so, libthostmduserapi_se.so 等)
set(CTP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp" CACHE PATH "CTP SDK root")
option(TQSDK_ENABLE_CTP "Build CTP gateway" OFF)

if(TQSDK_ENABLE_CTP)
    if(NOT EXISTS "${CTP_ROOT}/include/ThostFtdcTraderApi.h")
        message(FATAL_ERROR "CTP headers not found at ${CTP_ROOT}/include/")
    endif()

    # 根据平台确定库文件后缀
    if(WIN32)
        set(CTP_TRADER_LIB "${CTP_ROOT}/lib/thosttraderapi_se.lib")
        set(CTP_MD_LIB     "${CTP_ROOT}/lib/thostmduserapi_se.lib")
    else()
        set(CTP_TRADER_LIB "${CTP_ROOT}/lib/libthosttraderapi_se.so")
        set(CTP_MD_LIB     "${CTP_ROOT}/lib/libthostmduserapi_se.so")
    endif()

    add_library(ctp_trader SHARED IMPORTED)
    set_target_properties(ctp_trader PROPERTIES
        IMPORTED_LOCATION "${CTP_TRADER_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${CTP_ROOT}/include"
    )

    add_library(ctp_md SHARED IMPORTED)
    set_target_properties(ctp_md PROPERTIES
        IMPORTED_LOCATION "${CTP_MD_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${CTP_ROOT}/include"
    )

    add_library(tqsdk_ctp_gateway
        src/gateway/ctp/ctp_td_gateway.cpp
        src/gateway/ctp/ctp_md_gateway.cpp
        src/gateway/ctp/ctp_task_queue.cpp
        src/gateway/ctp/ctp_spi_impl.cpp
    )

    target_link_libraries(tqsdk_ctp_gateway PUBLIC
        tqsdk_core
        ctp_trader
        ctp_md
    )

    target_include_directories(tqsdk_ctp_gateway PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
endif()

# ---------- 示例可执行文件 ----------
option(TQSDK_BUILD_EXAMPLES "Build example executables" OFF)

if (TQSDK_BUILD_EXAMPLES)
    add_executable(tqsdk_demo examples/demo_strategy.cpp)
    target_link_libraries(tqsdk_demo PRIVATE tqsdk_core)

    if(TQSDK_ENABLE_CTP)
        target_link_libraries(tqsdk_demo PRIVATE tqsdk_ctp_gateway)
    endif()
endif()

# ---------- 测试 ----------
if(TQSDK_BUILD_TESTS)
    enable_testing()

    add_executable(tqsdk_tests
        tests/channel/channel_test.cpp
        tests/channel/channel_basic_test.cpp
        tests/channel/channel_bounded_test.cpp
        tests/channel/channel_close_test.cpp
        tests/channel/channel_multiplex_test.cpp
        tests/util/spawn_logged_test.cpp
    )
    target_include_directories(tqsdk_tests PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>
    )
    target_link_libraries(tqsdk_tests PRIVATE tqsdk_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(tqsdk_tests)
endif()