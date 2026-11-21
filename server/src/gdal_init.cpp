/// @file gdal_init.cpp
/// @brief GDAL/PROJ 初始化实现
#include "gdal_init.hpp"

#if defined(__has_include) && __has_include(<gdal.h>)
#include <gdal.h>
#else
#include <gdal/gdal.h>
#endif
#include <ogr_spatialref.h>

void gdalInit() {
    GDALAllRegister();
    /// PROJ 数据（proj.db + 网格文件）搜索路径：
    ///   1) REALITYMAP_PROJ_DATA —— 构建机 vcpkg 绝对路径（开发/CI 构建时定义）
    ///   2) "proj" —— 发布包内相对路径（<包根目录>/proj，随二进制一起分发）
    /// 相对路径以进程工作目录为基准，故发布包需在包根目录启动（与 web/public 挂载要求一致）。
    static const char *projPaths[] = {
#ifdef REALITYMAP_PROJ_DATA
        REALITYMAP_PROJ_DATA,
#endif
        "proj",
        nullptr
    };
    OSRSetPROJSearchPaths(projPaths);
}
