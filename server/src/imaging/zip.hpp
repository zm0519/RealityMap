/// @file zip.hpp
/// @brief 内存 ZIP 打包（miniz）：多文件 → 一个 zip 字节流
#ifndef IMAGING_ZIP_HPP
#define IMAGING_ZIP_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace imaging {

/// @brief 多文件内存 ZIP 打包器（miniz）
/// @details addMem 追加文件；finish 产出完整 zip 字节流（之后复用需重新 begin）。
class ZipWriter {
public:
    /// @brief 开始一次打包（重置内部状态）
    void begin();
    /// @brief 追加一个内存文件
    /// @param name zip 内文件名
    /// @param data 数据指针
    /// @param size 数据字节数
    void addMem(const std::string &name, const void *data, size_t size);
    /// @brief 追加一个磁盘文件
    /// @param name zip 内文件名
    /// @param path 源文件路径
    void addFile(const std::string &name, const std::filesystem::path &path);
    /// @brief 结束打包，返回 zip 全量字节
    /// @return 完整 zip 字节流
    std::vector<uint8_t> finish();

private:
    bool inited_ = false;      ///< 是否已 begin
    void *archive_ = nullptr;  ///< 实为 mz_zip_archive*，cpp 内部转型
};

} // namespace imaging
#endif // IMAGING_ZIP_HPP
