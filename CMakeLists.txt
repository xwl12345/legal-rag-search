cmake_minimum_required(VERSION 3.16)
project(legal_rag_search VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ── Qt resources (global QSS theme) ──
set(RESOURCES
    src/ui/theme.qrc
)

# ── Qt6 (fallback to Qt5) ──
# 必须在任何 add_executable / target_link_libraries 之前跑完：
# 否则下面的 target_link_libraries 引用 Qt5::Sql 这类 IMPORTED 目标时包还没找到。
find_package(Qt6 COMPONENTS Widgets Core Network Sql QUIET)
if(NOT Qt6_FOUND)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Core Network Sql)
    message(STATUS "Using Qt5")
else()
    message(STATUS "Using Qt6")
endif()

# ── Third-party header-only libraries ──
set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/nlohmann/single_include)
set(CPPJIEBA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/cppjieba/include)
set(LIMONP_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/limonp/include)

# ── Source files ──
# 引擎层（不含 Qt UI，测试目标复用）
set(SRC_ENGINE
    # Document parsing & tokenization
    src/document/parser.cpp
    src/document/tokenizer.cpp
    src/document/pdf_extractor.cpp
    src/document/ocr_client.cpp
    src/document/metadata.cpp

    # Index & search
    src/index/inverted_index.cpp
    src/index/bm25_ranker.cpp
    # Index persistence（T1：与 UI 解耦的落盘层）
    src/index/index_store.cpp

    # Vector retrieval
    src/vector/embedding.cpp
    src/vector/similarity.cpp

    # RAG pipeline
    src/rag/retriever.cpp
    src/rag/generator.cpp
)

# 存储层（T2：问答历史 SQLite；不含 Qt Widgets，测试目标复用）
set(SRC_STORE
    src/history/history_store.cpp
)

# Qt UI 层（T0 起统一由此三块组成：主题 / 组件 / 页面）
set(SRC_UI
    src/ui/app_theme.cpp
    src/ui/navigation_bar.cpp
    src/ui/placeholder_page.cpp
    src/ui/search_page.cpp
    src/ui/library_page.cpp
    src/ui/history_page.cpp
    src/ui/main_window.cpp
)

set(SOURCES
    src/main.cpp
    ${SRC_ENGINE}
    ${SRC_STORE}
    ${SRC_UI}
)

# ── Headers ──
set(HEADERS
    include/document/parser.h
    include/document/tokenizer.h
    include/document/ocr_client.h
    include/index/inverted_index.h
    include/index/bm25_ranker.h
    include/index/index_store.h
    include/vector/embedding.h
    include/vector/similarity.h
    include/rag/retriever.h
    include/rag/generator.h
    include/ui/main_window.h
    include/ui/app_theme.h
    include/ui/navigation_bar.h
    include/ui/placeholder_page.h
    include/ui/search_page.h
    include/ui/library_page.h
    include/ui/history_page.h
    include/config/app_config.h
    include/history/history_record.h
    include/history/history_store.h
)

# ── UI 骨架冒烟测试（离屏渲染，导出各页截图）──
add_executable(ui_smoke
    test/ui_smoke.cpp
    ${SRC_ENGINE}
    ${SRC_STORE}
    ${SRC_UI}
    ${HEADERS}
    ${RESOURCES}
)

target_include_directories(ui_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${JSON_INCLUDE_DIR}
    ${CPPJIEBA_INCLUDE_DIR}
    ${LIMONP_INCLUDE_DIR}
)

target_compile_definitions(ui_smoke PRIVATE
    CPPJIEBA_DICT_PATH="${CMAKE_SOURCE_DIR}/third_party/cppjieba/dict"
)

if(Qt6_FOUND)
    target_link_libraries(ui_smoke PRIVATE Qt6::Widgets Qt6::Core Qt6::Network Qt6::Sql)
else()
    target_link_libraries(ui_smoke PRIVATE Qt5::Widgets Qt5::Core Qt5::Network Qt5::Sql)
endif()

if(WIN32)
    target_link_libraries(ui_smoke PRIVATE ws2_32 z)
endif()

# ── Executable ──
add_executable(${PROJECT_NAME} src/main.cpp ${SRC_ENGINE} ${SRC_STORE} ${SRC_UI} ${HEADERS} ${RESOURCES})

# OCR helper is deployed next to the executable so the PDF fallback works
# outside the source-tree working directory.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/tools"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/pdf_ocr.py"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/tools/pdf_ocr.py"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${JSON_INCLUDE_DIR}
    ${CPPJIEBA_INCLUDE_DIR}
    ${LIMONP_INCLUDE_DIR}
)

# cppjieba dictionary path
target_compile_definitions(${PROJECT_NAME} PRIVATE
    CPPJIEBA_DICT_PATH="${CMAKE_SOURCE_DIR}/third_party/cppjieba/dict"
)

# ── Qt 链接（find_package 已在文件顶部统一跑完）──
if(Qt6_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets Qt6::Core Qt6::Network Qt6::Sql)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Widgets Qt5::Core Qt5::Network Qt5::Sql)
endif()

# ── SQLite (bundled with Qt, but add explicit link) ──
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE SQLite::SQLite3)
endif()

# ── Platform settings ──
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 z)
    # Qt DLL 使用动态 MinGW 运行时（它们需要 libstdc++-6.dll 等），
    # exe 也统一用动态链接，避免同一进程内出现两份 C++ 运行时导致符号冲突。
    # windeployqt 会自动部署所需 DLL，不需要静态链接。
    # （之前 -static 导致 "找不到程序入口点"）
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /O2)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -O3)
    endif()
endif()

# Focused regression for UTF-8 chunk boundaries and legal location queries.
add_executable(test_utf8_location_regression
    test/test_utf8_location_regression.cpp
    src/document/parser.cpp
    src/document/tokenizer.cpp
    src/document/pdf_extractor.cpp
    src/document/ocr_client.cpp
    src/document/metadata.cpp
    src/index/inverted_index.cpp
    src/index/bm25_ranker.cpp
    src/index/index_store.cpp
    src/vector/embedding.cpp
    src/vector/similarity.cpp
    src/rag/retriever.cpp
)
target_include_directories(test_utf8_location_regression PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${JSON_INCLUDE_DIR}
    ${CPPJIEBA_INCLUDE_DIR}
    ${LIMONP_INCLUDE_DIR}
)
target_compile_definitions(test_utf8_location_regression PRIVATE
    CPPJIEBA_DICT_PATH="${CMAKE_SOURCE_DIR}/third_party/cppjieba/dict"
)
if(Qt6_FOUND)
    target_link_libraries(test_utf8_location_regression PRIVATE
        Qt6::Core Qt6::Network z)
else()
    target_link_libraries(test_utf8_location_regression PRIVATE
        Qt5::Core Qt5::Network z)
endif()
if(WIN32)
    target_link_libraries(test_utf8_location_regression PRIVATE ws2_32)
endif()

# ── Unit tests ──
set(TEST_SOURCES
    test/test_main.cpp

    # Reuse core sources (without main + UI)
    src/document/parser.cpp
    src/document/tokenizer.cpp
    src/document/pdf_extractor.cpp
    src/document/ocr_client.cpp
    src/document/metadata.cpp
    src/index/inverted_index.cpp
    src/index/bm25_ranker.cpp
    src/index/index_store.cpp
    src/vector/embedding.cpp
    src/vector/similarity.cpp
    src/rag/retriever.cpp
    src/rag/generator.cpp
    ${SRC_STORE}
)

add_executable(run_tests ${TEST_SOURCES})

target_include_directories(run_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${JSON_INCLUDE_DIR}
    ${CPPJIEBA_INCLUDE_DIR}
    ${LIMONP_INCLUDE_DIR}
)

target_compile_definitions(run_tests PRIVATE
    CPPJIEBA_DICT_PATH="${CMAKE_SOURCE_DIR}/third_party/cppjieba/dict"
)

if(Qt6_FOUND)
    target_link_libraries(run_tests PRIVATE Qt6::Widgets Qt6::Core Qt6::Network Qt6::Sql)
else()
    target_link_libraries(run_tests PRIVATE Qt5::Widgets Qt5::Core Qt5::Network Qt5::Sql)
endif()

if(WIN32)
    target_link_libraries(run_tests PRIVATE ws2_32 z)
endif()

# ── 检索质量量化评测 ──
add_executable(eval_retrieval test/eval_retrieval.cpp
    src/document/parser.cpp
    src/document/tokenizer.cpp
    src/document/pdf_extractor.cpp
    src/document/ocr_client.cpp
    src/document/metadata.cpp
    src/index/inverted_index.cpp
    src/index/bm25_ranker.cpp
    src/index/index_store.cpp
    src/vector/embedding.cpp
    src/vector/similarity.cpp
    src/rag/retriever.cpp
)
target_include_directories(eval_retrieval PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${JSON_INCLUDE_DIR}
    ${CPPJIEBA_INCLUDE_DIR}
    ${LIMONP_INCLUDE_DIR}
)
target_compile_definitions(eval_retrieval PRIVATE
    CPPJIEBA_DICT_PATH="${CMAKE_SOURCE_DIR}/third_party/cppjieba/dict"
)
if(Qt6_FOUND)
    target_link_libraries(eval_retrieval PRIVATE Qt6::Core Qt6::Network z)
else()
    target_link_libraries(eval_retrieval PRIVATE Qt5::Core Qt5::Network z)
endif()
if(WIN32)
    target_link_libraries(eval_retrieval PRIVATE ws2_32)
endif()

# ── SQLite 驱动插件部署（T2，放在文件末尾：所有 target 都已定义）──
# Qt 把 QSQLITE 驱动做成插件放在 <Qt前缀>/plugins/sqldrivers/，程序运行时
# 从「exe 所在目录/sqldrivers/」去找。不部署就会报 "Driver not loaded"
# （实测症状：QSqlDatabase::drivers() 返回空列表，open() 一律失败），
# 问答历史页直接不可用。这一步等价于 windeployqt 的其中一环。
if(Qt6_FOUND)
    set(QT_CMAKE_DIR "${Qt6_DIR}")
else()
    set(QT_CMAKE_DIR "${Qt5_DIR}")
endif()
get_filename_component(QT_INSTALL_PREFIX "${QT_CMAKE_DIR}/../../.." ABSOLUTE)
set(QT_SQLITE_PLUGIN "${QT_INSTALL_PREFIX}/plugins/sqldrivers/qsqlite.dll")
if(EXISTS "${QT_SQLITE_PLUGIN}")
    foreach(_target ${PROJECT_NAME} run_tests ui_smoke)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${_target}>/sqldrivers"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_SQLITE_PLUGIN}" "$<TARGET_FILE_DIR:${_target}>/sqldrivers/qsqlite.dll"
        )
    endforeach()
    message(STATUS "SQLite driver plugin: ${QT_SQLITE_PLUGIN}")
else()
    message(WARNING "未找到 SQLite 驱动插件（${QT_SQLITE_PLUGIN}），问答历史落库将报 Driver not loaded")
endif()
