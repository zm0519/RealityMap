/// @file stitch.cpp
/// @brief 底图拼接实现：GDAL 解码 + 双线性合成 + 垂直翻转
#include "imaging/stitch.hpp"

#include "geo_math.hpp"

#if defined(__has_include) && __has_include(<gdal.h>)
#include <gdal.h>
#else
#include <gdal/gdal.h>
#endif

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace imaging {

namespace {

/// @brief 层名 → 目录名（与 config.tile_layers 约定一致，简化：直接同名映射 + 已知别名）
/// @param layer 层名（"vec"/"cva"/"img"）
/// @return 本地瓦片目录名
std::string layerDir(const std::string &layer) {
    if (layer == "vec") return "vec_tiles";
    if (layer == "img") return "img_tiles";
    return layer;                       // cva 等同名目录
}

/// @brief 解码一张瓦片为 RGBA；失败返回空
/// @param p 瓦片文件路径
/// @return RGBA 字节流（行主序），解码失败返回空
std::vector<uint8_t> decodeTile(const std::filesystem::path &p) {
    GDALDatasetH ds = GDALOpenEx(p.string().c_str(),
                                 GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!ds) return {};
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
    std::vector<uint8_t> rgba;
    if (GDALGetRasterCount(ds) >= 4) {
        rgba.resize((size_t)w * h * 4);
        GDALDatasetRasterIO(ds, GF_Read, 0, 0, w, h,
                            rgba.data(), w, h, GDT_Byte, 4, nullptr,
                            4, (GSpacing)w * 4, 1);
    } else if (GDALGetRasterCount(ds) >= 3) {
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        GDALDatasetRasterIO(ds, GF_Read, 0, 0, w, h,
                            rgb.data(), w, h, GDT_Byte, 3, nullptr,
                            3, (GSpacing)w * 3, 1);
        rgba.resize((size_t)w * h * 4, 255);
        for (size_t i = 0; i < rgb.size() / 3; ++i) {
            rgba[i * 4] = rgb[i * 3];
            rgba[i * 4 + 1] = rgb[i * 3 + 1];
            rgba[i * 4 + 2] = rgb[i * 3 + 2];
        }
    } else if (GDALGetRasterCount(ds) == 1) {
        /// 调色板（PLTE）PNG——天地图矢量/标注瓦片即此格式：读索引带 + 颜色表展开（含 alpha）
        GDALRasterBandH band = GDALGetRasterBand(ds, 1);
        std::vector<uint8_t> idx((size_t)w * h);
        GDALRasterIO(band, GF_Read, 0, 0, w, h, idx.data(), w, h, GDT_Byte, 0, 0);
        GDALColorTableH ct = GDALGetRasterColorTable(band);
        rgba.assign((size_t)w * h * 4, 0);
        if (ct) {
            const int n = GDALGetColorEntryCount(ct);
            for (size_t i = 0; i < idx.size(); ++i) {
                if (idx[i] < n) {
                    const GDALColorEntry *c = GDALGetColorEntry(ct, idx[i]);
                    rgba[i * 4] = c->c1; rgba[i * 4 + 1] = c->c2;
                    rgba[i * 4 + 2] = c->c3; rgba[i * 4 + 3] = c->c4;
                }
            }
        }
    }
    GDALClose(ds);
    return rgba;
}

} // namespace

StitchResult stitchMap(const StitchParams &p) {
    if (p.layers.empty() || p.outRes < 16 || p.outRes > 16384)
        throw std::runtime_error("stitch: 参数无效");

    /// 选区 bbox（与 heightmap 同语义，angle=0——overlay 场景必须轴对齐）
    const geo::Bbox b = geo::coveringBbox(p.lng, p.lat, p.sizeKm, 0.0);
    StitchResult out;
    out.res = p.outRes;
    out.rgb.assign((size_t)p.outRes * p.outRes * 3, 255);   // 白底

    /// 选区四角的全局像素坐标（zoom 级 256px/瓦片体系）
    const double gx0 = geo::lonToTileX(b.w, p.zoom) * 256.0;
    const double gx1 = geo::lonToTileX(b.e, p.zoom) * 256.0;
    const double gy0 = geo::latToTileY(b.n, p.zoom) * 256.0;
    const double gy1 = geo::latToTileY(b.s, p.zoom) * 256.0;
    if (gx1 <= gx0 || gy1 <= gy0) throw std::runtime_error("stitch: 范围退化");

    /// 逐行合成：对每个输出行，算它落在哪些瓦片行（就近采样）
    const double sx = (gx1 - gx0) / p.outRes, sy = (gy1 - gy0) / p.outRes;
    std::vector<std::vector<uint8_t>> rowTiles;   // 当前行涉及的瓦片缓存
    int cachedTy = -2, tx0 = 0, tx1 = 0;

    for (int r = 0; r < p.outRes; ++r) {
        const double gy = gy0 + sy * (r + 0.5);
        const int ty = (int)std::floor(gy / 256.0);
        if (ty != cachedTy) {                       // 换瓦片行：重解码缓存
            cachedTy = ty;
            const int tx_first = (int)std::floor(gx0 / 256.0);
            const int tx_last = (int)std::floor((gx1 - 1e-9) / 256.0);
            tx0 = tx_first;
            tx1 = tx_last;
            rowTiles.assign((size_t)(tx_last - tx_first + 1) * p.layers.size(), {});
            for (size_t L = 0; L < p.layers.size(); ++L)
                for (int tx = tx_first; tx <= tx_last; ++tx) {
                    const auto path = p.tileRoot
                                      / (p.useTileCache ? ("tile_cache/" + p.layers[L]) : layerDir(p.layers[L]))
                                      / std::to_string(p.zoom)
                                      / std::to_string(tx) / (std::to_string(ty) + ".png");
                    auto &dst = rowTiles[(size_t)(tx - tx_first) * p.layers.size() + L];
                    if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 400)
                        dst = decodeTile(path);
                    else ++out.missingTiles;
                }
        }
        const double fy = std::fmod(gy, 256.0);
        const int py = (int)std::clamp(std::floor(fy), 0.0, 255.0);

        for (int c = 0; c < p.outRes; ++c) {
            const double gx = gx0 + sx * (c + 0.5);
            const int tx = (int)std::floor(gx / 256.0);
            if (tx < tx0 || tx > tx1) continue;

            /// 双线性插值坐标（tile 内浮点位置）
            const double fx = std::clamp(std::fmod(gx, 256.0), 0.0, 255.0);
            const double fyB = std::clamp(fy, 0.0, 255.0);
            const int px0 = (int)fx, py0 = (int)fyB;
            const int px1 = std::min(px0 + 1, 255), py1 = std::min(py0 + 1, 255);
            const double tx_frac = fx - px0, ty_frac = fyB - py0;

            for (size_t L = 0; L < p.layers.size(); ++L) {          // 逐层 alpha 合成
                const auto &t = rowTiles[(size_t)(tx - tx0) * p.layers.size() + L];
                if (t.empty()) continue;

                /// 双线性取 RGBA（4 邻域加权）
                double rr = 0, gg = 0, bb = 0, aa = 0;
                const uint8_t *p00 = &t[((size_t)py0 * 256 + px0) * 4];
                const uint8_t *p10 = &t[((size_t)py0 * 256 + px1) * 4];
                const uint8_t *p01 = &t[((size_t)py1 * 256 + px0) * 4];
                const uint8_t *p11 = &t[((size_t)py1 * 256 + px1) * 4];
                const double w00 = (1 - tx_frac) * (1 - ty_frac);
                const double w10 = tx_frac * (1 - ty_frac);
                const double w01 = (1 - tx_frac) * ty_frac;
                const double w11 = tx_frac * ty_frac;
                rr = p00[0]*w00 + p10[0]*w10 + p01[0]*w01 + p11[0]*w11;
                gg = p00[1]*w00 + p10[1]*w10 + p01[1]*w01 + p11[1]*w11;
                bb = p00[2]*w00 + p10[2]*w10 + p01[2]*w01 + p11[2]*w11;
                aa = p00[3]*w00 + p10[3]*w10 + p01[3]*w01 + p11[3]*w11;

                const uint8_t a = (uint8_t)aa;
                if (a < 4) continue;                                // 近全透明跳过
                uint8_t *d = &out.rgb[((size_t)r * p.outRes + c) * 3];
                if (a > 251) {
                    d[0] = (uint8_t)rr; d[1] = (uint8_t)gg; d[2] = (uint8_t)bb;
                } else {
                    const double k = aa / 255.0;
                    d[0] = (uint8_t)(rr * k + d[0] * (1 - k));
                    d[1] = (uint8_t)(gg * k + d[1] * (1 - k));
                    d[2] = (uint8_t)(bb * k + d[2] * (1 - k));
                }
            }
        }
    }
    /// 游戏引擎纹理 UV 原点在左下（Y 向上），PNG 原点在左上（行 0=北）
    /// Image Overlay mod 把 PNG 当贴图用 → 需要垂直翻转让南北正确
    {
        std::vector<uint8_t> flipped(out.rgb.size());
        const size_t rowBytes = (size_t)p.outRes * 3;
        for (int y = 0; y < p.outRes; ++y)
            std::memcpy(&flipped[(size_t)(p.outRes - 1 - y) * rowBytes],
                        &out.rgb[(size_t)y * rowBytes], rowBytes);
        out.rgb = std::move(flipped);
    }
    return out;
}

} // namespace imaging
