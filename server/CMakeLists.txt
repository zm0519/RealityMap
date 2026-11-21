find_package(GDAL CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(PNG CONFIG REQUIRED)
find_package(GeographicLib CONFIG REQUIRED)
find_path(OSMIUM_INCLUDE_DIRS "osmium/version.hpp")   # libosmium 为 header-only：仅提供头文件路径
find_package(ZLIB REQUIRED)                            # PBF 块为 zlib 压缩
find_package(BZip2 REQUIRED)                           # libosmium any_input 无条件引用
find_package(EXPAT REQUIRED)                           # 同上：XML(.osm) 解析器

# 核心模块库：算法/数据层（server 与 tests 共用；每课新增 .cpp 记得加进 SOURCES）
add_library(realitymap_core STATIC
    src/dem/vrt_builder.cpp
    src/dem/dem_sources.cpp
    src/core/resample.cpp
    src/core/heightmap_gen.cpp
    src/core/preview.cpp
    src/imaging/png16.cpp
    src/imaging/zip.cpp
    src/imaging/stitch.cpp
    src/net/tile_fetch.cpp
    src/osm/buildings.cpp
    src/osm/roads.cpp
    src/gdal_init.cpp
)
target_include_directories(realitymap_core PUBLIC src ${OSMIUM_INCLUDE_DIRS})
target_link_libraries(realitymap_core PUBLIC
    GDAL::GDAL
    nlohmann_json::nlohmann_json
    GeographicLib::GeographicLib
    miniz::miniz
    ZLIB::ZLIB
    BZip2::BZip2
    EXPAT::EXPAT
)

# PROJ 数据库定位：vcpkg 只部署 DLL 不部署 proj.db，显式把路径编进二进制
# （Linux/系统 GDAL 能自动找到 proj.db，此宏为空、走系统路径）
find_file(PROJ_DB_PATH proj.db
    PATHS ${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share/proj
    NO_DEFAULT_PATH)
if (PROJ_DB_PATH)
    get_filename_component(PROJ_DATA_DIR ${PROJ_DB_PATH} DIRECTORY)
    target_compile_definitions(realitymap_core PUBLIC REALITYMAP_PROJ_DATA="${PROJ_DATA_DIR}")
    message(STATUS "PROJ data: ${PROJ_DATA_DIR}")
endif()

add_executable(realitymap-server
    src/main.cpp
)

target_link_libraries(realitymap-server PRIVATE
    realitymap_core
    httplib::httplib
    miniz::miniz
    CURL::libcurl
    PNG::PNG
)

# 测试（doctest）
find_package(doctest CONFIG REQUIRED)
enable_language(CXX)
include(${doctest_DIR}/doctest.cmake OPTIONAL RESULT_VARIABLE doctest_helpers)
if (doctest_helpers)
    # doctest_discover_tests 等辅助函数可用
endif()

if (BUILD_TESTING)
    add_executable(realitymap_tests
        tests/test_main.cpp
    )
    target_link_libraries(realitymap_tests PRIVATE
        realitymap_core
        doctest::doctest
    )
    add_test(NAME realitymap_tests COMMAND realitymap_tests)
endif()
