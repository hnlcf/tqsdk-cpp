# TQSDK Qt desktop UI (independent frontend process)
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(tqsdk_ui VERSION 0.1.0 LANGUAGES CXX)
endif()
#
# This target intentionally does NOT link tqsdk_core. The UI talks to
# tqsdk-node through REST + WebSocket so it can be built, upgraded and
# shipped independently from the C++ backend.

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
qt_standard_project_setup()

set(TQSDK_UI_SOURCES
    src/main.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/net/NodeClient.cpp
    src/net/NodeClient.h
    src/net/Protocol.cpp
    src/net/Protocol.h
    src/net/HttpWsNodeClient.cpp
    src/net/HttpWsNodeClient.h
    src/model/MarketStore.cpp
    src/model/MarketStore.h
    src/model/QuoteTableModel.cpp
    src/model/QuoteTableModel.h
    src/model/InstrumentTableModel.cpp
    src/model/InstrumentTableModel.h
    src/model/TradingModels.cpp
    src/model/TradingModels.h
    src/chart/KLineChartWidget.cpp
    src/chart/KLineChartWidget.h
    src/widgets/TablePanel.cpp
    src/widgets/TablePanel.h
)

qt_add_executable(tqsdk-ui ${TQSDK_UI_SOURCES})

target_include_directories(tqsdk-ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(tqsdk-ui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
)

target_compile_features(tqsdk-ui PRIVATE cxx_std_20)

# Static vcpkg Qt does not automatically link the Wayland platform plugin.
# Keep XCB for XWayland fallback and explicitly import the native Wayland QPA.
qt_import_plugins(tqsdk-ui INCLUDE Qt6::QWaylandIntegrationPlugin)

set_target_properties(tqsdk-ui PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS tqsdk-ui
        BUNDLE  DESTINATION .
        RUNTIME DESTINATION bin)

option(TQSDK_BUILD_UI_TESTS "Build Qt UI tests" OFF)
if(TQSDK_BUILD_UI_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    qt_add_executable(tqsdk-ui-tests
        tests/ProtocolTest.cpp
        src/net/Protocol.cpp
        src/net/Protocol.h
    )
    target_include_directories(tqsdk-ui-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(tqsdk-ui-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(NAME tqsdk-ui-protocol COMMAND tqsdk-ui-tests)

    qt_add_executable(tqsdk-ui-model-tests
        tests/ModelTest.cpp
        src/net/Protocol.cpp
        src/net/Protocol.h
        src/model/MarketStore.cpp
        src/model/MarketStore.h
        src/model/QuoteTableModel.cpp
        src/model/QuoteTableModel.h
        src/model/InstrumentTableModel.cpp
        src/model/InstrumentTableModel.h
        src/model/TradingModels.cpp
        src/model/TradingModels.h
    )
    target_include_directories(tqsdk-ui-model-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(tqsdk-ui-model-tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Test
    )
    add_test(NAME tqsdk-ui-models COMMAND tqsdk-ui-model-tests)
endif()
