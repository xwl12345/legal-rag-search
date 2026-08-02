cmake_minimum_required(VERSION 3.16)
project(rag_search_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Third-party header-only libraries ──
set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/nlohmann/single_include)
set(CPPJIEBA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/cppjieba/include)
set(LIMONP_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/limonp/include)

# ── Source files ──
set(SOURCES
    src/main.cpp

    # Document parsing & tokenization
    src/document/parser.cpp
    src/document/tokenizer.cpp
    src/document/pdf_extractor.cpp
    src/document/metadata.cpp

    # Index & search
    src/index/inverted_index.cpp
    src/index/bm25_ranker.cpp

    # Vector retrieval
    src/vector/embedding.cpp
    src/vector/similarity.cpp

    # RAG pipeline
    src/rag/retriever.cpp
    src/rag/generator.cpp

    # Qt UI
    src/ui/main_window.cpp
)

# ── Headers ──
set(HEADERS
    include/document/parser.h
    include/document/tokenizer.h
    include/index/inverted_index.h
    include/index/bm25_ranker.h
    include/vector/embedding.h
    include/vector/similarity.h
    include/rag/retriever.h
    include/rag/generator.h
    include/ui/main_window.h
    include/config/app_config.h
)

# ── Executable ──
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

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

# ── Qt6 (fallback to Qt5) ──
find_package(Qt6 COMPONENTS Widgets Core Network QUIET)
if(NOT Qt6_FOUND)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Core Network)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Widgets Qt5::Core Qt5::Network)
    message(STATUS "Using Qt5")
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets Qt6::Core Qt6::Network)
    message(STATUS "Using Qt6")
endif()

# ── SQLite (bundled with Qt, but add explicit link) ──
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE SQLite::SQLite3)
endif()

# ── Platform settings ──
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 z)
    # 静态链接 MinGW 全部运行时（libstdc++/libgcc/libwinpthread），
    # 彻底消除 DLL 版本冲突。Qt 库仍为动态链接（无静态 .a 可用）。
    if(MINGW)
        target_link_options(${PROJECT_NAME} PRIVATE -static)
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /O2)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -O3)
    endif()
endif()

# ── Unit tests ──
set(TEST_SOURCES
    test/test_main.cpp

    # Reuse core sources (without main + UI)
    src/document/parser.cpp
    src/document/tokenizer.cpp
    src/document/pdf_extractor.cpp
    src/document/metadata.cpp
    src/index/inverted_index.cpp
    src/index/bm25_ranker.cpp
    src/vector/embedding.cpp
    src/vector/similarity.cpp
    src/rag/retriever.cpp
    src/rag/generator.cpp
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
    target_link_libraries(run_tests PRIVATE Qt6::Widgets Qt6::Core Qt6::Network)
else()
    target_link_libraries(run_tests PRIVATE Qt5::Widgets Qt5::Core Qt5::Network)
endif()

if(WIN32)
    target_link_libraries(run_tests PRIVATE ws2_32 z)
    if(MINGW)
        target_link_options(run_tests PRIVATE -static)
    endif()
endif()
