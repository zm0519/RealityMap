/// @file zip.cpp
/// @brief 内存 ZIP 打包实现（miniz）
#include "imaging/zip.hpp"

#include <miniz/miniz.h>
#include <stdexcept>

namespace imaging {

void ZipWriter::begin() {
    if (inited_) throw std::runtime_error("ZipWriter: 重复 begin");
    auto *z = new mz_zip_archive{};
    if (!mz_zip_writer_init_heap(z, 0, 0)) {
        delete z;
        throw std::runtime_error("miniz: writer 初始化失败");
    }
    archive_ = z;
    inited_ = true;
}

void ZipWriter::addMem(const std::string &name, const void *data, size_t size) {
    if (!inited_) throw std::runtime_error("ZipWriter: 未 begin");
    if (!mz_zip_writer_add_mem(static_cast<mz_zip_archive *>(archive_),
                               name.c_str(), data, size, MZ_DEFAULT_COMPRESSION))
        throw std::runtime_error("miniz: 添加文件失败: " + name);
}

void ZipWriter::addFile(const std::string &name, const std::filesystem::path &path) {
    FILE *f = fopen(path.string().c_str(), "rb");
    if (!f)
        throw std::runtime_error("ZipWriter: 读文件失败: " + path.string());
    std::vector<uint8_t> buf;
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    addMem(name, buf.data(), buf.size());
}

std::vector<uint8_t> ZipWriter::finish() {
    if (!inited_) throw std::runtime_error("ZipWriter: 未 begin");
    auto *z = static_cast<mz_zip_archive *>(archive_);
    void *buf = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(z, &buf, &size) || !buf) {
        mz_zip_writer_end(z);
        delete z;
        archive_ = nullptr;
        inited_ = false;
        throw std::runtime_error("miniz: 归档失败");
    }
    std::vector<uint8_t> out(static_cast<uint8_t *>(buf),
                             static_cast<uint8_t *>(buf) + size);
    mz_free(buf);
    mz_zip_writer_end(z);
    delete z;
    archive_ = nullptr;
    inited_ = false;
    return out;
}

} // namespace imaging
