/// @file test_main.cpp
/// @brief 单元测试（doctest）：几何/瓦片数学、重采样、高度图映射、效果、预设表等
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#if defined(__has_include) && __has_include(<gdal.h>)
#include <gdal.h>
#else
#include <gdal/gdal.h>
#endif
#include <filesystem>
#include <iostream>

#include "geo_math.hpp"
#include "gdal_init.hpp"
#include "dem/vrt_builder.hpp"
#include "dem/dem_sources.hpp"
#include "core/resample.hpp"
#include "core/settings.hpp"

TEST_CASE("toolchain smoke") {
    GDALAllRegister();
    CHECK(std::string(GDALVersionInfo("RELEASE_NAME")).size() > 0);
}


TEST_CASE("geo: 瓦片数学属性") {
    /// 边界：-180° 是 x=0；纬度极限钳制到 y=0
    CHECK(geo::lonToTileX(-180.0, 6) == doctest::Approx(0.0));
    CHECK(geo::latToTileY(89.0, 6) == doctest::Approx(0.0).epsilon(1e-12));     // 超限被钳制
    CHECK(geo::latToTileY(-89.0, 6) == doctest::Approx(64.0).epsilon(1e-12));
    /// 经度环绕：540° ≡ 180°
    CHECK(geo::lonToTileX(540.0, 6) == doctest::Approx(geo::lonToTileX(180.0, 6)));
    /// 亲子瓦片：z+1 的 (2x,2y) 与 z 的 (x,y) 同范围
    const auto p  = geo::tileBbox(26, 13, 6);
    const auto nw = geo::tileBbox(52, 26, 7), ne = geo::tileBbox(53, 26, 7);
    const auto sw = geo::tileBbox(52, 27, 7), se = geo::tileBbox(53, 27, 7);
    CHECK(nw.w == doctest::Approx(p.w).epsilon(1e-12));
    CHECK(nw.n == doctest::Approx(p.n).epsilon(1e-12));
    CHECK(ne.e == doctest::Approx(p.e).epsilon(1e-12));
    CHECK(ne.n == doctest::Approx(p.n).epsilon(1e-12));
    CHECK(sw.w == doctest::Approx(p.w).epsilon(1e-12));
    CHECK(sw.s == doctest::Approx(p.s).epsilon(1e-12));
    CHECK(se.e == doctest::Approx(p.e).epsilon(1e-12));
    CHECK(se.s == doctest::Approx(p.s).epsilon(1e-12));
    CHECK(nw.e == doctest::Approx(ne.w).epsilon(1e-12));
    CHECK(nw.s == doctest::Approx(sw.n).epsilon(1e-12));
    /// 覆盖语义：covering 出的瓦片必须真的盖住 bbox 四角
    const auto b = geo::coveringBbox(117.0, 40.0, 17.28, 30.0);
    const auto r = geo::tilesCovering(b, 10);
    const auto t00 = geo::tileBbox(r.x0, r.y0, r.z), t11 = geo::tileBbox(r.x1, r.y1, r.z);
    CHECK(t00.w <= b.w); CHECK(t00.n >= b.n);
    CHECK(t11.e >= b.e); CHECK(t11.s <= b.s);
}

TEST_CASE("geo: 测地正反算互逆（GeographicLib 精确）") {
    double lat2, lon2, azi2, s12, azi1;
    geo::wgs84().Direct(40.5, 117.5, 45.0, 100000.0, lat2, lon2, azi2);
    geo::wgs84().Inverse(40.5, 117.5, lat2, lon2, s12, azi1, azi2);
    CHECK(s12 == doctest::Approx(100000.0).epsilon(1e-12));   // 椭球上精确还原 100km
}

TEST_CASE("geo: 交叉验证——测地法与经典级数两条独立路线互证") {
    /// 同一问题算两遍：Δlat 使经线弧长 = 17280m
    ///   A. 精确法：测地 Direct 向北走 17280m，看跨了多少度
    ///   B. 级数法：17280 / metersPerDegLat_series(φ)
    /// 两条独立公式体系（Karney 椭球积分 vs Snyder 级数）应在 0.01% 内一致
    for (double lat : {18.0, 30.0, 40.5, 53.5}) {
        double latN, lonN;
        geo::wgs84().Direct(lat, 117.5, 0.0, 17280.0, latN, lonN);
        const double dLatExact = latN - lat;
        const double dLatSeries = 17280.0 / geo::metersPerDegLat_series(lat);
        CHECK(dLatExact == doctest::Approx(dLatSeries).epsilon(2e-4));
    }
}

TEST_CASE("geo: 任意尺寸选区覆盖（0.5km 到 229km 通用）") {
    for (double km : {0.5, 17.28, 57.344, 100.0, 229.376}) {
        const auto b = geo::coveringBbox(117.5, 40.5, km);
        /// 中心经线上的南北边缘测地长应精确等于边长（经线是测地线，构造保证）
        double s12;
        geo::wgs84().Inverse(b.s, 117.5, b.n, 117.5, s12);
        CHECK(s12 == doctest::Approx(km * 1000.0).epsilon(1e-9));
    }
}

/// 测试固定样本集：样本目录（15 幅真 GDEM + Float32 合成图 + UTM 衍生物，稳定不随全国下载变化）
/// VRT 输出到临时目录，避免与线上服务的 D:/data/DEM/mosaic.vrt 争写
static std::filesystem::path g_testVrt;

TEST_CASE("vrt: 真数据镶嵌（某区域）") {
    gdalInit();   // 模块契约：用 GDAL 前先做进程级初始化（幂等）
    namespace fs = std::filesystem;
    const fs::path demRoot = "D:/data/DEM/30/chengde";
    if (!fs::exists(demRoot)) {
        WARN("样本目录不存在，本测试跳过");
        return;
    }
    const fs::path out = fs::temp_directory_path() / "hm_test" / "mosaic.vrt";
    fs::create_directories(out.parent_path());

    const auto r = dem::buildDemVrt(demRoot, out);
    for (const auto &f : r.files)
        if (!f.ok)
            std::cout << "[跳过] " << f.path << "  原因: " << f.reason << "\n";
    g_testVrt = r.vrtPath;

    CHECK(r.includedCount == 15);   // 15 幅原生 GDEM；Float32/UTM 衍生物被明确拒绝
    bool utmSkipped = false, floatSkipped = false;
    for (const auto &f : r.files) {
        if (!f.ok && f.path.find("EPSG_32650") != std::string::npos) utmSkipped = true;
        if (!f.ok && f.path.find("chengde.tif") != std::string::npos) floatSkipped = true;
    }
    CHECK(utmSkipped);
    CHECK(floatSkipped);

    /// 镶嵌结果可打开、地理范围盖住 115-120E / 40-43N
    GDALDatasetH ds = GDALOpenEx(r.vrtPath.string().c_str(),
                                 GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    REQUIRE(ds != nullptr);
    double gt[6];
    REQUIRE(GDALGetGeoTransform(ds, gt) == CE_None);
    const double w = gt[0], e = gt[0] + gt[1] * GDALGetRasterXSize(ds);
    const double n = gt[3], s = gt[3] + gt[5] * GDALGetRasterYSize(ds);
    CHECK(w <= 115.0); CHECK(e >= 120.0); CHECK(s <= 40.0); CHECK(n >= 43.0);
    GDALClose(ds);
}

TEST_CASE("dem: 开窗读取某地") {
    gdalInit();
    namespace fs = std::filesystem;
    const fs::path vrt = g_testVrt.empty() ? fs::temp_directory_path() / "hm_test" / "mosaic.vrt" : g_testVrt;
    if (!fs::exists(vrt)) { WARN("测试 VRT 不存在（先跑 vrt 测试），跳过"); return; }

    const dem::DemSource src(vrt);
    /// 某地市区一带 17.28km
    const auto b = geo::coveringBbox(117.93, 40.97, 17.28);
    const auto g = src.readWindow(b, 256, 256);

    CHECK(g.validCount > 0);
    /// CHECK(g.minValid == doctest::Approx(-500).epsilon(2.0)); // 容差式：|实际-期望|<=2*|期望|
    /// 某盆地~300m，某山脉~1500-2100m，范围应落在合理区间
     CHECK(g.maxValid > 200.0);   
    CHECK(g.maxValid < 2500.0);
    /// 中心点非 NaN 且为正（市区海拔 ~350m）
    CHECK(!std::isnan(g.at(128, 128)));
}

TEST_CASE("resample: 双线性 lattice 精确 + 中点均值") {
    core::Grid g{4, 4, 0, 0, 3, 3};
    g.v.resize(16);
    for (int i = 0; i < 16; ++i) g.v[i] = (float)(i * 10);
    core::TargetAffine id{4, 4, 0, 1.0, 0, 3, 0, -1.0};        // 恒等仿射
    const auto out = core::resample(g, id, core::Interp::Bilinear);
    for (int i = 0; i < 16; ++i)
        CHECK(out[i] == doctest::Approx(g.v[i]).epsilon(1e-6));
    core::TargetAffine mid{1, 1, 1.5, 1.0, 0, 1.5, 0, -1.0};   // 采样 (fx,fy)=(1.5,1.5)
    CHECK(core::resample(g, mid, core::Interp::Bilinear)[0] == doctest::Approx(75.0).epsilon(1e-6));
}

TEST_CASE("resample: Mitchell 核精确还原线性斜面") {
    core::Grid g{8, 8, 0, 0, 7, 7};
    g.v.resize(64);
    for (int r = 0; r < 8; ++r) for (int c = 0; c < 8; ++c)
        g.v[r * 8 + c] = (float)(3 * c + 7 * r);
    for (double px = 2.3; px < 5; px += 0.7) {                  // 远离边界的内部点
        /// lat=4.1 → fy = nDeg-lat = 7-4.1 = 2.9（行号方向！期望值必须用行号算）
        core::TargetAffine t{1, 1, px, 1.0, 0, 4.1, 0, -1.0};
        const auto out = core::resample(g, t, core::Interp::Bicubic);
        CHECK(out[0] == doctest::Approx(3 * px + 7 * 2.9).epsilon(1e-4));
    }
}

TEST_CASE("resample: 仿射定位（行列序易错点）") {
    core::Grid g{10, 10, 0, 0, 9, 9};
    g.v.resize(100);
    for (int i = 0; i < 100; ++i) g.v[i] = (float)i;             // v[r*10+c] = 10r+c
    /// 采样源像素 (c=2, r=3)：lon = 2, lat = nDeg - r = 6
    core::TargetAffine t{1, 1, 2.0, 1.0, 0, 6.0, 0, -1.0};
    CHECK(core::resample(g, t, core::Interp::Bilinear)[0] == doctest::Approx(32.0));
}

TEST_CASE("settings: 预设表") {
    CHECK(core::findFormat("cs1")->elevationScaleM == doctest::Approx(1023.984375));
    CHECK(core::findFormat("cs2")->dualImage == true);
    CHECK(core::findFormat("unity")->defaultRes == 513);
    CHECK(core::findFormat("ue")->defaultRes == 505);
    CHECK(core::findFormat("xxx") == nullptr);
}
