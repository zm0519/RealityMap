/// @file dem_sources.cpp
/// @brief DEM 数据源实现：GDAL 窗口读取与单点查询
#include "dem/dem_sources.hpp"
#include <stdexcept>

namespace dem{
DemSource::DemSource(const std::filesystem::path &vrtPath)
{
    ds_ = GDALOpenEx(vrtPath.string().c_str(),
                    GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if(!ds_) throw std::runtime_error("打开DEM失败：" + vrtPath.string());
    if(GDALGetGeoTransform(ds_, gt_) != CE_None) { 
        GDALClose(ds_); 
        ds_ = nullptr;
        throw std::runtime_error("DEM 无 GeoTransform: " + vrtPath.string());
    }
    rasterX_ = GDALGetRasterXSize(ds_);
    rasterY_ = GDALGetRasterYSize(ds_);
}

DemSource::~DemSource()
{
    if(ds_) GDALClose(ds_);
}

ElevationGrid DemSource::readWindow(const geo::Bbox &b, int outW, int outH) const
{
    /// 1) bbox → 数据集浮点像素坐标（注意 gt_[5] 为负）
    const double x0 = (b.w - gt_[0]) / gt_[1];
    const double x1 = (b.e - gt_[0]) / gt_[1];
    const double y0 = (b.n - gt_[3]) / gt_[5];
    const double y1 = (b.s - gt_[3]) / gt_[5];

    ElevationGrid g;
    g.bbox = b;
    g.width = outW;
    g.height = outH;
    g.values.assign((std::size_t)outW * outH, std::nanf(""));

    /// 2) 与数据集求交——无交集直接返回全 NaN 网格
    const double ix0 = std::max(x0, 0.0), iy0 = std::max(y0, 0.0);
    const double ix1 = std::min(x1, (double)rasterX_), iy1 = std::min(y1, (double)rasterY_);
    if(ix0 >= ix1 || iy0 >= iy1) return g;

    /// 3) 交集像素 → 输出网格像素的比例映射（这是最需要想清楚的一段！）
    ///    交集在 bbox 里的相对位置 → 对应输出网格中的子矩形
    const double sx = (double)outW / (x1 - x0);
    const double sy = (double)outH / (y1 - y0);
    int ox = (int)std::floor((ix0 - x0) * sx);
    int oy = (int)std::floor((iy0 - y0) * sy);
    int ow = (int)std::ceil((ix1 - x0) * sx) - ox;
    int oh = (int)std::ceil((iy1 - y0) * sy) - oy;
    /// 输出矩形钳制在网格内：窗口与数据集边界精确重合时，ceil 的浮点误差
    /// 会把 ow/oh 抬高 1 像素导致越界写（2026-11-14 实案：10km 请求触发
    /// Debug 断言"vector subscript out of range"，Release 下为无声越界写）
    ox = std::clamp(ox, 0, outW - 1);
    oy = std::clamp(oy, 0, outH - 1);
    ow = std::min(ow, outW - ox);
    oh = std::min(oh, outH - oy);

    /// 4) 浮点窗口 RasterIO（双线性）
    /// 整数窗口：floor/ceil 覆盖语义（宁多读边缘 1 像素≈30m，不留缝）
    const int sxOff = (int)std::floor(ix0);
    const int syOff = (int)std::floor(iy0);
    const int sxSize = (int)std::ceil(ix1) - sxOff;
    const int sySize = (int)std::ceil(iy1) - syOff;

    GDALRasterIOExtraArg args;
    INIT_RASTERIO_EXTRA_ARG(args);
    args.eResampleAlg = GRIORA_Bilinear;   // 源窗口→缓冲 重采样生效

    std::vector<float> buf((std::size_t)ow * oh);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        CPLErr err = GDALDatasetRasterIOEx(ds_, GF_Read,
            sxOff, syOff, sxSize, sySize,     // ★ 整数窗口
            buf.data(), ow, oh, GDT_Float32, 1, nullptr,
            sizeof(float), 0, 0, &args);
        if(err != CE_None) throw std::runtime_error("RasterIO 失败");
    }

    /// 5) 拷入输出网格 + 哨兵→NaN + 统计
    float mn = std::nanf(""), mx = std::nanf(""); long valid{0};
    for(int r = 0; r < oh; ++r){
        for(int c = 0; c < ow; ++c){
            float v = buf[r * ow + c];
            if(v == GDEM_NODATA || v < -9000.0f) v = std::nanf("");
            if(std::isnan(v)) continue;
            g.values[(std::size_t)(oy + r) * outW + (ox + c)] = v;
            ++valid;
            if(std::isnan(mn) || v < mn) mn = v;
            if(std::isnan(mx) || v > mx) mx = v;
        }
    }
    g.validCount = valid;
    if(valid) { g.minValid = mn; g.maxValid = mx; }
    return g;
}

float DemSource::elevationAt(double lng, double lat) const {
    const double fx = (lng - gt_[0]) / gt_[1];
    const double fy = (gt_[3] - lat) / -gt_[5];            // y 步长为负
    if (fx < 0 || fy < 0 || fx >= rasterX_ || fy >= rasterY_) return std::nanf("");
    float v = std::nanf("");
    std::lock_guard<std::mutex> lk(mtx_);
    GDALRasterIO(GDALGetRasterBand(ds_, 1), GF_Read,
                 (int)fx, (int)fy, 1, 1, &v, 1, 1, GDT_Float32, 0, 0);
    if (v == GDEM_NODATA || v < -9000.0f) return std::nanf("");
    return v;
}

geo::Bbox DemSource::coverage() const
{
    return { gt_[0], gt_[3] + gt_[5] * rasterY_, gt_[0] + gt_[1] * rasterX_, gt_[3]};
}

} // dem