// =============================================================================
// tcTcvMappedFile.cpp - platform code for TcvMappedFile
// =============================================================================

#include <cstring>
#include "tcTcvMappedFile.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tcx::tcv {

#ifdef _WIN32

bool TcvMappedFile::open(const std::filesystem::path& path) {
    close();
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        return false;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        return false;
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    fileHandle_ = file;
    mappingHandle_ = mapping;
    data_ = static_cast<const uint8_t*>(view);
    size_ = static_cast<uint64_t>(size.QuadPart);
    return true;
}

void TcvMappedFile::close() {
    if (data_) UnmapViewOfFile(data_);
    if (mappingHandle_) CloseHandle(static_cast<HANDLE>(mappingHandle_));
    if (fileHandle_) CloseHandle(static_cast<HANDLE>(fileHandle_));
    data_ = nullptr;
    size_ = 0;
    mappingHandle_ = nullptr;
    fileHandle_ = nullptr;
}

#else

bool TcvMappedFile::open(const std::filesystem::path& path) {
    close();
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }
    void* view = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // the mapping keeps the file referenced
    if (view == MAP_FAILED) return false;
    data_ = static_cast<const uint8_t*>(view);
    size_ = static_cast<uint64_t>(st.st_size);
    return true;
}

void TcvMappedFile::close() {
    if (data_) munmap(const_cast<uint8_t*>(data_), static_cast<size_t>(size_));
    data_ = nullptr;
    size_ = 0;
}

#endif

} // namespace tcx::tcv
