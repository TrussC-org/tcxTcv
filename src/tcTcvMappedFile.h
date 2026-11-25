#pragma once

// =============================================================================
// tcTcvMappedFile.h - read-only memory-mapped file for TcvPlayer
// =============================================================================
// The player reads every frame straight from the mapping: LZ4 decompresses from
// the OS page cache with no read() copy into a staging buffer. The platform
// code lives in tcTcvMappedFile.cpp so <windows.h> stays out of this header.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace tcx::tcv {

class TcvMappedFile {
public:
    TcvMappedFile() = default;
    ~TcvMappedFile() { close(); }
    TcvMappedFile(const TcvMappedFile&) = delete;
    TcvMappedFile& operator=(const TcvMappedFile&) = delete;

    // Maps the whole file read-only. Returns false (and stays closed) on failure.
    bool open(const std::filesystem::path& path);
    void close();

    bool isOpen() const { return data_ != nullptr; }
    uint64_t size() const { return size_; }

    // Pointer to [offset, offset + bytes) in the mapping, or nullptr if that
    // range runs past the end of the file.
    const uint8_t* at(uint64_t offset, uint64_t bytes) const {
        if (!data_ || offset > size_ || bytes > size_ - offset) return nullptr;
        return data_ + offset;
    }

    // Copies a POD value at offset. Returns false past the end of the file.
    template <class T>
    bool read(uint64_t offset, T& out) const {
        const uint8_t* p = at(offset, sizeof(T));
        if (!p) return false;
        std::memcpy(&out, p, sizeof(T));
        return true;
    }

private:
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
#ifdef _WIN32
    void* fileHandle_ = nullptr;     // HANDLE
    void* mappingHandle_ = nullptr;  // HANDLE
#endif
};

} // namespace tcx::tcv
