/// @file png16.cpp
/// @brief PNG 编码实现：16-bit 灰度与 8-bit RGB
#include "png16.hpp"
#include <png.h>
#include <stdexcept>

namespace imaging{
static void pngThrow(png_structp, png_const_charp msg) { throw std::runtime_error(std::string("libpng: ") + msg); }

std::vector<uint8_t> encodeGray16(const std::vector<uint16_t> &pix, int w, int h)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, pngThrow, nullptr);
    if(!png) throw std::runtime_error("png_create_write_struct 失败");
    png_infop info = png_create_info_struct(png);
    if(!info) { png_destroy_write_struct(&png, nullptr); throw std::runtime_error("png_create_info_struct 失败"); }

    std::vector<uint8_t> out;
    auto writeCb = [](png_structp p, png_bytep data, png_size_t len){
        auto* buf = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(p));
        buf->insert(buf->end(), data, data+len);
    };
    auto flushCb = [](png_structp){};
    png_set_write_fn(png, &out, writeCb, flushCb);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 16, PNG_COLOR_TYPE_GRAY,
                PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    /// 行缓冲：16-bit 大端（PNG 规范）
    std::vector<png_byte> row((std::size_t)w * 2);
    for(int y=0; y<h; ++y){
        for(int x=0; x<w; ++x){
            const uint16_t v = pix[(std::size_t)y * w + x];
            row[(std::size_t)x * 2] = (png_byte)(v >> 8);
            row[(std::size_t)x * 2 + 1] = (png_byte)(v & 0xFF);
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return out;
}

bool writeGray16(const std::filesystem::path &path, const std::vector<uint16_t> &pix, int w, int h)
{
    try
    {
        const auto bytes = encodeGray16(pix, w, h);
        std::filesystem::create_directories(path.parent_path());
        FILE* f = fopen(path.string().c_str(), "wb");
        if(!f) return false;
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        return true;  
    }
    catch(const std::exception& e)
    {
        return false;
    }
}

std::vector<uint8_t> encodeRgb8(const std::vector<uint8_t>& rgb, int w, int h) {
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, pngThrow, nullptr);
    if (!png) throw std::runtime_error("png_create_write_struct 失败");
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); throw std::runtime_error("png_create_info_struct 失败"); }
    std::vector<uint8_t> out;
    auto writeCb = [](png_structp p, png_bytep data, png_size_t len) {
        static_cast<std::vector<uint8_t> *>(png_get_io_ptr(p))->insert(
            static_cast<std::vector<uint8_t> *>(png_get_io_ptr(p))->end(), data, data + len);
    };
    png_set_write_fn(png, &out, writeCb, [](png_structp) {});
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < h; ++y)
        png_write_row(png, const_cast<png_bytep>(&rgb[(size_t)y * w * 3]));
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return out;
}

} // namespace imaging