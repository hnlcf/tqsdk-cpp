cmake_minimum_required(VERSION 3.20)
project(tqsdk_cpp VERSION 0.1.0 LANGUAGES CXX)

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
find_package(tomlplusplus CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

# GoogleTest (仅在启用测试时)
option(TQSDK_BUILD_TESTS "Build tests" OFF)
if(TQSDK_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# ---------- 核心库 (不依赖 CTP) ----------
add_library(tqsdk_core
    src/api/tq_api.cpp
    src/ipc/uds_channel.cpp
    src/ipc/tcp_channel.cpp
    src/ipc/ipc_session.cpp
    src/ipc/ipc_client.cpp
    src/ipc/remote_md_gateway.cpp
    src/ipc/remote_td_gateway.cpp
    src/ipc/gateway_server.cpp
    src/serve/http_server.cpp
    src/serve/ws_stream.cpp
    src/serve/json_convert.cpp
    src/config/tqapi_factory.cpp
    src/data/diff_merge_engine.cpp
    src/gateway/sim/sim_td_gateway.cpp
    src/gateway/tq/tq_md_protocol.cpp
    src/gateway/tq/tq_md_gateway.cpp
    src/util/log.cpp
    # src/data/data_manager.cpp
    # src/data/diff_merge_engine.cpp
    # src/channel/channel.cpp
    # src/gateway/tq_md_gateway.cpp
    # src/gateway/tq_td_gateway.cpp
    # src/gateway/sim_md_gateway.cpp
)

target_precompile_headers(tqsdk_core PRIVATE include/tqsdk/pch.h)

# 保留 TRACE/DEBUG 级日志语句（运行时再按级别过滤）。
# 未定义时 spdlog 默认 SPDLOG_LEVEL_INFO，会把 SPDLOG_TRACE/DEBUG 编译期剔除。
target_compile_definitions(tqsdk_core PUBLIC SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

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
    tomlplusplus::tomlplusplus
    spdlog::spdlog
    CLI11::CLI11
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

# ---------- 两进程：tqsdk-gw / tqsdk-node ----------
option(TQSDK_BUILD_APPS "Build tqsdk-gw and tqsdk-node executables" ON)

if(TQSDK_BUILD_APPS)
    add_executable(tqsdk-gw apps/gw_main.cpp)
    target_link_libraries(tqsdk-gw PRIVATE tqsdk_core)

    add_executable(tqsdk-node apps/node_main.cpp)
    target_link_libraries(tqsdk-node PRIVATE tqsdk_core)
endif()

# ---------- 示例可执行文件 ----------
option(TQSDK_BUILD_EXAMPLES "Build example executables" OFF)

if (TQSDK_BUILD_EXAMPLES)
    add_executable(tqsdk_demo examples/demo.cpp)
    target_link_libraries(tqsdk_demo PRIVATE tqsdk_core)

    if(TQSDK_ENABLE_CTP)
        target_link_libraries(tqsdk_demo PRIVATE tqsdk_ctp_gateway)
    endif()
endif()

# ---------- 测试 ----------
if(TQSDK_BUILD_TESTS)
    enable_testing()

    add_executable(tqsdk_tests
        tests/main.cpp
        tests/msg_test.cpp
        tests/channel/channel_test.cpp
        tests/channel/channel_basic_test.cpp
        tests/channel/channel_bounded_test.cpp
        tests/channel/channel_close_test.cpp
        tests/channel/channel_multiplex_test.cpp
        tests/data/data_manager_test.cpp
        tests/data/diff_merge_test.cpp
        tests/data/entity_store_test.cpp
        tests/ipc/uds_channel_test.cpp
        tests/ipc/tcp_channel_test.cpp
        tests/ipc/protocol_test.cpp
        tests/ipc/protocol_e2e_test.cpp
        tests/ipc/ipc_session_test.cpp
        tests/ipc/ipc_proxy_test.cpp
        tests/ipc/gateway_server_test.cpp
        tests/config/tqapi_factory_test.cpp
        tests/util/serde_test.cpp
        tests/util/spawn_logged_test.cpp
        tests/util/log_test.cpp
        tests/e2e/mock_e2e_test.cpp
        tests/e2e/mock_e2e_edges_test.cpp
        tests/e2e/sim_td_e2e_test.cpp
        tests/gateway/tq_md_protocol_test.cpp
        tests/serve/ws_stream_test.cpp
        tests/e2e/tq_md_network_test.cpp
    )
    target_include_directories(tqsdk_tests PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>
    )
    target_link_libraries(tqsdk_tests PRIVATE tqsdk_core GTest::gtest)

    include(GoogleTest)
    gtest_discover_tests(tqsdk_tests)
endif()

# ==========================================================
# 安装规则
# ==========================================================
if(TQSDK_BUILD_APPS)
    install(TARGETS tqsdk-gw tqsdk-node
            RUNTIME DESTINATION bin)
endif()

# tqsdk_core：默认是静态库(ARCHIVE)；若以后 BUILD_SHARED_LIBS=ON，
# 则 .so/.dll 分别走 LIBRARY / RUNTIME，所以三种都写上最稳。
install(TARGETS tqsdk_core
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# 头文件：include/ 下的内容原样铺到 <prefix>/include/
install(DIRECTORY include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.ipp")

# CTP 网关（可选）
if(TQSDK_ENABLE_CTP)
    install(TARGETS tqsdk_ctp_gateway
            ARCHIVE DESTINATION lib
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION bin)
    # IMPORTED 目标不能被 install(TARGETS)，必须手动拷 CTP 自带的动态库，
    # 否则用户解压后 tqsdk-gw 运行时会找不到 libthost*_se.so
    install(FILES "${CTP_TRADER_LIB}" "${CTP_MD_LIB}" DESTINATION lib)
    # Windows 上还需把 thosttraderapi_se.dll / thostmduserapi_se.dll 一起装进来
endif()

# ==========================================================
# CPack：生成 .tar.gz
# ==========================================================
set(CPACK_PACKAGE_NAME "tqsdk_cpp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "TQSDK C++ core library, gateway and node")
set(CPACK_PACKAGE_VENDOR "TQSDK")
set(CPACK_PACKAGE_CONTACT "dev@example.com")

set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# 包内前缀：解压后得到 <包名>/opt/tqsdk/{bin,lib,include}
# 若想解压出来直接是 bin/lib/include，把它设为 "/"（绿色分发常用）
set(CPACK_PACKAGING_INSTALL_PREFIX "/")

# 源码包先关掉，避免 cpack --config 时额外生成 source tarball
set(CPACK_SOURCE_GENERATOR "")

include(CPack)