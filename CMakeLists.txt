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
find_package(SQLiteCpp CONFIG REQUIRED)

# GoogleTest (仅在启用测试时)
option(TQSDK_BUILD_TESTS "Build tests" OFF)
if(TQSDK_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# ---------- 核心动态库（CTP 开启时直接并入同一 .so） ----------
add_library(tqsdk_core SHARED
    src/api/tq_api.cpp
    src/api/tq_api_order.cpp
    src/ipc/uds_channel.cpp
    src/ipc/tcp_channel.cpp
    src/ipc/ipc_session.cpp
    src/ipc/ipc_client.cpp
    src/ipc/remote_md_gateway.cpp
    src/ipc/remote_td_gateway.cpp
    src/ipc/gateway_server.cpp
    src/ipc/gateway_subscription_book.cpp
    src/ipc/gateway_server_pumps.cpp
    src/ipc/gateway_server_control.cpp
    src/serve/http_server.cpp
    src/serve/http_routes.cpp
    src/serve/snapshot_service.cpp
    src/instrument/instrument_catalog.cpp
    src/serve/ws_stream.cpp
    src/serve/ws_entity_filter.cpp
    src/serve/json_convert.cpp
    src/config/tqapi_factory.cpp
    src/data/diff_merge_engine.cpp
    src/data/diff_merge_quotes.cpp
    src/data/diff_merge_trade.cpp
    src/data/diff_merge_positions.cpp
    src/data/diff_merge_klines.cpp
    src/data/kline_store.cpp
    src/gateway/sim/sim_td_gateway.cpp
    src/gateway/tq/tq_md_protocol.cpp
    src/gateway/tq/tq_md_gateway.cpp
    src/util/log.cpp
    src/util/http_download.cpp
    src/auth/tq_auth.cpp
    src/trading/holiday_calendar.cpp
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
set_target_properties(tqsdk_core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
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

# SQLiteCpp 只服务于 node 的 K 线缓存实现，不进入公共头文件/安装依赖。
target_link_libraries(tqsdk_core PRIVATE SQLiteCpp)

# ---------- CTP 网关 (可选，仅当找到 CTP SDK 时编译) ----------
# 将 CTP 的 .so/.dll 和头文件放在 third_party/ctp/ 下：
#   third_party/ctp/include/   (ThostFtdc*.h)
#   third_party/ctp/lib/       (libthosttraderapi_se.so, libthostmduserapi_se.so 等)
set(CTP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp" CACHE PATH "CTP SDK root")
option(TQSDK_ENABLE_CTP "Build CTP gateway" OFF)

if(TQSDK_ENABLE_CTP)
    if(EXISTS "${CTP_ROOT}/include/ThostFtdcTraderApi.h")
        set(CTP_INCLUDE_DIR "${CTP_ROOT}/include")
    elseif(EXISTS "${CTP_ROOT}/ThostFtdcTraderApi.h")
        set(CTP_INCLUDE_DIR "${CTP_ROOT}")
    else()
        message(FATAL_ERROR "CTP headers not found at ${CTP_ROOT}/include/ or ${CTP_ROOT}/")
    endif()

    # 兼容两种 SDK 布局：lib/libthost*.so 与根目录/thost*.so。
    if(WIN32)
        set(CTP_TRADER_LIB "${CTP_ROOT}/lib/thosttraderapi_se.lib")
        set(CTP_MD_LIB     "${CTP_ROOT}/lib/thostmduserapi_se.lib")
    else()
        if(EXISTS "${CTP_ROOT}/lib/libthosttraderapi_se.so")
            set(CTP_TRADER_LIB "${CTP_ROOT}/lib/libthosttraderapi_se.so")
        else()
            set(CTP_TRADER_LIB "${CTP_ROOT}/thosttraderapi_se.so")
        endif()
        if(EXISTS "${CTP_ROOT}/lib/libthostmduserapi_se.so")
            set(CTP_MD_LIB "${CTP_ROOT}/lib/libthostmduserapi_se.so")
        else()
            set(CTP_MD_LIB "${CTP_ROOT}/thostmduserapi_se.so")
        endif()
    endif()

    # CTP SDK 的 .so 没有 SONAME，且文件名没有 lib 前缀。若按绝对路径链接，
    # DT_NEEDED 会写入构建机路径；若直接 -l 又找不到文件。
    # 解决：在构建目录生成 lib 前缀的符号链接用于链接，安装时也用同名文件，
    # 这样 DT_NEEDED 只保留文件名，运行时由 $ORIGIN 找到同目录的 CTP 库。
    function(tq_ctp_lib_name out_var lib_path)
        get_filename_component(name "${lib_path}" NAME)
        if(name MATCHES "^lib")
            set("${out_var}" "${name}" PARENT_SCOPE)
        else()
            set("${out_var}" "lib${name}" PARENT_SCOPE)
        endif()
    endfunction()

    tq_ctp_lib_name(CTP_TRADER_SONAME "${CTP_TRADER_LIB}")
    tq_ctp_lib_name(CTP_MD_SONAME "${CTP_MD_LIB}")

    set(CTP_LINK_DIR "${CMAKE_CURRENT_BINARY_DIR}/ctp-libs")
    file(MAKE_DIRECTORY "${CTP_LINK_DIR}")
    set(CTP_TRADER_LINK "${CTP_LINK_DIR}/${CTP_TRADER_SONAME}")
    set(CTP_MD_LINK "${CTP_LINK_DIR}/${CTP_MD_SONAME}")
    if(NOT EXISTS "${CTP_TRADER_LINK}")
        file(CREATE_LINK "${CTP_TRADER_LIB}" "${CTP_TRADER_LINK}" SYMBOLIC)
    endif()
    if(NOT EXISTS "${CTP_MD_LINK}")
        file(CREATE_LINK "${CTP_MD_LIB}" "${CTP_MD_LINK}" SYMBOLIC)
    endif()

    add_library(ctp_trader SHARED IMPORTED)
    set_target_properties(ctp_trader PROPERTIES
        IMPORTED_LOCATION "${CTP_TRADER_LINK}"
        IMPORTED_NO_SONAME TRUE
        IMPORTED_SONAME "${CTP_TRADER_SONAME}"
        INTERFACE_INCLUDE_DIRECTORIES "${CTP_INCLUDE_DIR}"
    )

    add_library(ctp_md SHARED IMPORTED)
    set_target_properties(ctp_md PROPERTIES
        IMPORTED_LOCATION "${CTP_MD_LINK}"
        IMPORTED_NO_SONAME TRUE
        IMPORTED_SONAME "${CTP_MD_SONAME}"
        INTERFACE_INCLUDE_DIRECTORIES "${CTP_INCLUDE_DIR}"
    )

    # CTP 源文件直接编译进唯一的 tqsdk_core 动态库，不再生成第二个库。
    target_sources(tqsdk_core PRIVATE
        src/gateway/ctp/ctp_td_gateway.cpp
        src/gateway/ctp/ctp_td_orders.cpp
        src/gateway/ctp/ctp_td_queries.cpp
        src/gateway/ctp/ctp_query_pipeline.cpp
        src/gateway/ctp/ctp_md_gateway.cpp
        src/gateway/ctp/ctp_task_queue.cpp
        src/gateway/ctp/ctp_order_store.cpp
        src/gateway/ctp/ctp_spi_impl.cpp
    )

    target_link_libraries(tqsdk_core PRIVATE ctp_trader ctp_md)
    target_compile_definitions(tqsdk_core PUBLIC TQSDK_ENABLE_CTP=1)
    target_include_directories(tqsdk_core PUBLIC
        $<BUILD_INTERFACE:${CTP_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
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
        tests/data/kline_store_test.cpp
        tests/data/kline_cache_integration_test.cpp
        tests/data/diff_merge_test.cpp
        tests/data/entity_store_test.cpp
        tests/ipc/uds_channel_test.cpp
        tests/ipc/tcp_channel_test.cpp
        tests/ipc/protocol_test.cpp
        tests/ipc/protocol_e2e_test.cpp
        tests/ipc/stream_seq_test.cpp
        tests/ipc/ipc_session_test.cpp
        tests/ipc/command_ack_cache_test.cpp
        tests/ipc/gateway_subscription_book_test.cpp
        tests/ipc/ipc_proxy_test.cpp
        tests/ipc/gateway_server_test.cpp
        tests/config/tqapi_factory_test.cpp
        tests/auth/tq_auth_test.cpp
        tests/util/json_value_test.cpp
        tests/util/serde_test.cpp
        tests/util/spawn_logged_test.cpp
        tests/util/log_test.cpp
        tests/e2e/mock_e2e_test.cpp
        tests/e2e/mock_e2e_edges_test.cpp
        tests/e2e/sim_td_e2e_test.cpp
        tests/gateway/tq_md_protocol_test.cpp
        tests/instrument/instrument_catalog_test.cpp
        tests/serve/ws_protocol_test.cpp
        tests/serve/ws_stream_test.cpp
        tests/serve/ws_entity_filter_test.cpp
        tests/serve/catalog_http_test.cpp
        tests/serve/threading_test.cpp
        tests/serve/readiness_test.cpp
        tests/trading/trading_calendar_test.cpp
        tests/trading/tqapi_gateway_fault_test.cpp
        tests/e2e/tq_md_network_test.cpp
    )
    target_include_directories(tqsdk_tests PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests>
    )
    target_precompile_headers(tqsdk_tests PRIVATE tests/test_pch.h)
    target_link_libraries(tqsdk_tests PRIVATE tqsdk_core GTest::gtest)

    if(TQSDK_ENABLE_CTP)
        target_sources(tqsdk_tests PRIVATE
            tests/gateway/ctp_mapping_test.cpp
        tests/gateway/ctp_query_pipeline_test.cpp
            tests/gateway/ctp_order_store_test.cpp
            tests/gateway/ctp_fault_injection_test.cpp
        )
    endif()

    include(GoogleTest)
    gtest_discover_tests(tqsdk_tests)
endif()

# ==========================================================
# 安装规则
# ==========================================================
# 让安装后的 bin 能找到 ../lib 下的 libtqsdk_core.so，
# 让 lib 下的 libtqsdk_core.so 能找到同目录的 CTP SDK 动态库。
if(UNIX)
    set_target_properties(tqsdk_core PROPERTIES INSTALL_RPATH "$ORIGIN")
    if(TQSDK_BUILD_APPS)
        set_target_properties(tqsdk-gw tqsdk-node PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    endif()
endif()

if(TQSDK_BUILD_APPS)
    install(TARGETS tqsdk-gw tqsdk-node
            RUNTIME DESTINATION bin)
endif()

# 唯一的核心动态库（CTP 开启时 CTP 目标文件已编入本库）。
install(TARGETS tqsdk_core
        EXPORT tqsdkTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/tqsdkConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/tqsdkConfig.cmake"
    INSTALL_DESTINATION lib/cmake/tqsdk
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/tqsdkConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT tqsdkTargets
        FILE tqsdkTargets.cmake
        NAMESPACE tqsdk::
        DESTINATION lib/cmake/tqsdk)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/tqsdkConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/tqsdkConfigVersion.cmake"
        DESTINATION lib/cmake/tqsdk)

# 头文件：include/ 下的内容原样铺到 <prefix>/include/
install(DIRECTORY include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.ipp")

# CTP 是唯一需要随包分发的第三方运行时库；其头文件也一并安装，
# 这样安装后的 tqsdk/gateway/ctp/*.h 仍可被下游编译。
if(TQSDK_ENABLE_CTP)
    if(WIN32)
        install(FILES "${CTP_TRADER_LIB}" "${CTP_MD_LIB}" DESTINATION bin)
    else()
        # 安装为带 lib 前缀的名字；与 tqsdk_core.so 的 DT_NEEDED 一致。
        install(FILES "${CTP_TRADER_LIB}" DESTINATION lib RENAME "${CTP_TRADER_SONAME}")
        install(FILES "${CTP_MD_LIB}" DESTINATION lib RENAME "${CTP_MD_SONAME}")
    endif()
    install(DIRECTORY "${CTP_INCLUDE_DIR}/"
            DESTINATION include
            FILES_MATCHING PATTERN "*.h")
endif()

# 示例配置与文档；brokers.json 可能含柜台信息，不随包强制分发。
install(FILES config/gw.toml config/node.toml
        DESTINATION share/tqsdk/config)
install(FILES README.md docs/README.md docs/ARCHITECTURE.md docs/MODULES.md
              docs/FLOWS.md docs/STATE_MACHINES.md docs/PROTOCOLS.md docs/OPERATIONS.md
              docs/CTP.md docs/SOAK_PERF.md docs/WEB_FRONTEND.md docs/WEB_INDICATORS.md docs/TQ_AUTH.md docs/INSTRUMENTS.md
              docs/KLINE_CACHE.md docs/SIMNOW_ACCEPTANCE.md docs/ROADMAP.md
        DESTINATION share/doc/tqsdk)

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

# 绿色分发：包内直接是 bin/lib/include，不再套一层 <包名>/ 目录。
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

# 源码包先关掉，避免 cpack --config 时额外生成 source tarball
set(CPACK_SOURCE_GENERATOR "")

include(CPack)
