/// @file gdal_init.hpp
/// @brief GDAL/PROJ 进程级初始化
#ifndef SERVER_GDAL_INIT_HPP
#define SERVER_GDAL_INIT_HPP

/// @brief GDAL/PROJ 进程级初始化（幂等，尽早调用一次）
/// @details  - GDALAllRegister 注册全部驱动
///           - vcpkg 构建下显式指定 proj.db 搜索路径（DLL 旁不会部署数据文件）
void gdalInit();

#endif // SERVER_GDAL_INIT_HPP
