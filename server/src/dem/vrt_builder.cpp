/// @file vrt_builder.cpp
/// @brief DEM VRT 构建实现：递归扫描、格式校验、镶嵌成逻辑大图
#include "dem/vrt_builder.hpp"

#include <gdal_utils.h>
#include <ogr_spatialref.h> // OGRSpatialReference
#if defined(__has_include) && __has_include(<gdal.h>)
#include <gdal.h>
#else
#include <gdal/gdal.h>
#endif

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace dem{
/// @brief 校验单个文件；ok 时填报告并返回 true
/// @param p 文件路径
/// @param rep 体检报告（就地填充）
/// @return 是否通过校验（可纳入镶嵌）
static bool inspectFile(const std::filesystem::path& p, FileReport& rep){
    GDALDatasetH ds = GDALOpenEx(p.string().c_str(), 
                                GDAL_OF_RASTER | GDAL_OF_READONLY,
                                nullptr, nullptr, nullptr);
    if(!ds) { rep.reason = "GDAL 无法打开"; return false; }  
    
    bool ok = false;
    do{
        if(GDALGetRasterCount(ds) != 1) { rep.reason = "波段数 != 1"; break; }

        double gt[6] = {0}; // GeoTransform：6 元组 [原点x, x步长, x旋转, 原点y, y旋转, y步长]——像素坐标↔地理坐标的仿射
        if(GDALGetGeoTransform(ds, gt) != CE_None
            || gt[1] <= 0 || gt[5] >= 0){ // y 步长必须为负
                rep.reason = "GeoTransform 无效"; break;
        }
        const GDALDataType dt = GDALGetRasterDataType(GDALGetRasterBand(ds, 1));
        if (dt != GDT_Int16) {
            rep.reason = "波段类型非 Int16（DEM 镶像要求统一 Int16）";
            break;
        }
        const OGRSpatialReferenceH srs = GDALGetSpatialRef(ds);
        if(!srs || !OSRIsGeographic(srs)) { rep.reason = "非地理坐标系"; break; }
        const char *auth = OSRGetAuthorityName(srs, nullptr); // 返回 "EPSG"（此处只取码）
        const char* code = OSRGetAuthorityCode(srs, nullptr);
        const bool isEpsg4326 = auth && code && std::strcmp(auth, "EPSG") == 0 && std::strcmp(code, "4326") == 0;
        rep.epsg = code ? std::atoi(code) : 0;
        if(!isEpsg4326) { rep.reason = "非 EPSG:4326(是 " + std::to_string(rep.epsg) + ")"; break; }

        rep.width = GDALGetRasterXSize(ds);
        rep.height = GDALGetRasterYSize(ds);
        rep.resDeg = gt[1];
        int hasNd = 0;
        const double nd = GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &hasNd);
        rep.hasNodata = hasNd != 0;
        rep.nodata = nd;
        ok = true;
    }while (false);
    
    GDALClose(ds);
    rep.ok = ok;
    return ok;
}
BuildResult buildDemVrt(const std::filesystem::path& demRoot, const std::filesystem::path& vrtOut){
    if(!std::filesystem::exists(demRoot)){ throw std::runtime_error("DEM 目录不存在：" + demRoot.string()); }

    BuildResult result;
    result.vrtPath = vrtOut;
    std::vector<std::string> sources;
    for(const auto& entry : std::filesystem::recursive_directory_iterator(demRoot)){
        if(!entry.is_regular_file()) continue;
        if(entry.path().extension() != ".tif") continue;
        if(entry.path().filename() == "mosaic.vrt") continue;

        FileReport rep;
        rep.path = entry.path().string();
        inspectFile(entry.path(), rep);
        if(rep.ok) { ++result.includedCount; sources.push_back(rep.path); }
        else ++result.skippedCount;
        result.files.push_back(std::move(rep));
    }
    if(sources.empty()) throw std::runtime_error("没有可用的4326 DEM文件： " + demRoot.string());

    /// 生成镶嵌
    std::vector<const char*> srcPtrs;
    srcPtrs.reserve(sources.size());
    for(const auto& s : sources) srcPtrs.push_back(s.c_str());

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("-resolution"));
    argv.push_back(const_cast<char*>("highest")); // 混入 90m 数据时取最细分辨率
    argv.push_back(nullptr);

    GDALBuildVRTOptions* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    if(!opts) throw std::runtime_error("GDALBuildVRTOptionsNew 失败");

    int usageErr = 0;
    GDALDatasetH vrt = GDALBuildVRT(vrtOut.string().c_str(), (int)srcPtrs.size(),
                                    nullptr,    // 已打开的源句柄（我们传路径）
                                    srcPtrs.data(),
                                    opts, &usageErr);
    GDALBuildVRTOptionsFree(opts);
    if(!vrt) throw std::runtime_error("GDALBuildVrt 失败");
    GDALClose(vrt);
    return result;                                    
}

} // namespace dem