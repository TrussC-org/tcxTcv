#pragma once

// =============================================================================
// tcTcvPlayer.h - TCVC video player (v5: chunked LZ4 parallel decode)
// =============================================================================

// DEBUG: Enable profiling to see where time is spent
// #define TCV_PROFILE

#include <TrussC.h>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>

#include "tcTcvEncoder.h"  // For TcvHeader and constants
#include "tcTcvMappedFile.h"
#include <tc/sound/tcSound.h>  // For audio playback

namespace tcx::tcv {

// ---------------------------------------------------------------------------
// TcvChunkWorkers - persistent worker threads for the per-chunk LZ4 decode
// ---------------------------------------------------------------------------
// Each player owns one set. The threads are created once (load / setDecodeThreads)
// and reused for every I-frame. Spawning fresh std::threads per frame cost more
// than the decompression itself, and on Windows every short-lived thread left a
// little process heap behind (a looping 1080p player grew ~3.5 MB/min).
//
// run() hands out indices [0, count) one at a time; the calling thread works
// too, so N workers give N+1 decoders. With no workers it just runs inline.
class TcvChunkWorkers {
public:
    TcvChunkWorkers() = default;
    ~TcvChunkWorkers() { stop(); }
    TcvChunkWorkers(const TcvChunkWorkers&) = delete;
    TcvChunkWorkers& operator=(const TcvChunkWorkers&) = delete;

    void start(int workers) {
        stop();
        quit_ = false;
        for (int i = 0; i < workers; ++i) threads_.emplace_back([this] { loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
        threads_.clear();
    }

    int workerCount() const { return static_cast<int>(threads_.size()); }

    // Calls fn(i) for every i in [0, count) and returns when all are done.
    template <class Fn>
    void run(int count, Fn& fn) {
        if (threads_.empty() || count <= 1) {
            for (int i = 0; i < count; ++i) fn(i);
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        job_ = [](void* ctx, int i) { (*static_cast<Fn*>(ctx))(i); };
        ctx_ = &fn;
        count_ = count;
        next_ = 0;
        finished_ = 0;
        lock.unlock();
        wake_.notify_all();
        lock.lock();
        drain(lock);  // the caller decodes chunks too
        done_.wait(lock, [this] { return finished_ == count_; });
        count_ = 0;
        next_ = 0;
        job_ = nullptr;
        ctx_ = nullptr;
    }

private:
    // mutex_ is held on entry and on return.
    void drain(std::unique_lock<std::mutex>& lock) {
        while (next_ < count_) {
            const int i = next_++;
            auto job = job_;
            void* ctx = ctx_;
            lock.unlock();
            job(ctx, i);
            lock.lock();
            if (++finished_ == count_) done_.notify_all();
        }
    }

    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return quit_ || next_ < count_; });
            if (quit_) return;
            drain(lock);
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    bool quit_ = false;
    void (*job_)(void*, int) = nullptr;
    void* ctx_ = nullptr;
    int count_ = 0, next_ = 0, finished_ = 0;
};

// ---------------------------------------------------------------------------
// TcvPlayer - Plays TCVC encoded video
// ---------------------------------------------------------------------------
class TcvPlayer : public tc::VideoPlayerBase {
public:
    // Debug block types for visualization (v3: Skip or BC7 only)
    enum class DebugBlockType : uint8_t {
        None,    // I-frame or no debug
        Skip,    // Copy from reference
        BC7      // BC7 encoded
    };

    TcvPlayer() = default;
    ~TcvPlayer() { close(); }

    // Non-copyable
    TcvPlayer(const TcvPlayer&) = delete;
    TcvPlayer& operator=(const TcvPlayer&) = delete;

    // Debug mode
    void setDebug(bool enabled) { debugMode_ = enabled; }
    bool isDebug() const { return debugMode_; }

    // Override setSpeed to allow negative values (reverse playback)
    void setSpeed(float speed) {
        speed_ = speed;  // Allow any value including negative
        if (initialized_) {
            setSpeedImpl(speed_);
        }
    }

    // Threads used to decode the LZ4 chunks of an I-frame, including the calling
    // thread (1 = no worker threads). 0 = auto (half the hardware threads).
    // Can be changed at any time.
    void setDecodeThreads(int threads) {
        decodeThreads_ = std::max(0, threads);
        if (initialized_) startWorkers();
    }
    int getDecodeThreads() const { return workers_.workerCount() + 1; }

    // Performance stats
    double getDecodeTimeMs() const {
        return decodeTimeMs_;
    }
    void resetStats() {
        decodeTimeMs_ = 0.0;
    }

    // Draw debug overlay (call after drawing video)
    void drawDebugOverlay(float x, float y, float scale = 1.0f) {
        if (!debugMode_ || debugBlockTypes_.empty()) return;

        float blockSize = TCV_BLOCK_SIZE * scale;

        tc::noFill();
        tc::setColor(1.0f, 0.0f, 0.0f, 0.8f);  // Red for BC7 blocks

        for (int by = 0; by < blocksY_; by++) {
            for (int bx = 0; bx < blocksX_; bx++) {
                int idx = by * blocksX_ + bx;
                // Only draw BC7 blocks (Skip blocks are unchanged from reference)
                if (debugBlockTypes_[idx] != DebugBlockType::BC7) continue;

                float rx = x + bx * blockSize;
                float ry = y + by * blockSize;
                tc::drawRect(rx + 0.5f, ry + 0.5f, blockSize - 1, blockSize - 1);
            }
        }

        tc::fill();
        tc::setColor(1.0f);
    }

    // =========================================================================
    // Load / Close
    // =========================================================================

    tc::LoadResult load(const std::filesystem::path& path) override {
        if (initialized_) {
            close();
        }

        // Map the whole file: frames are decoded straight from the OS page
        // cache, with no read() copy into a staging buffer.
        if (!file_.open(path)) {
            tc::logError("TcvPlayer") << "Failed to open file: " << path.string();
            return tc::LoadResult::fail(tc::LoadError::FileNotFound, "cannot open " + path.string());
        }

        // Read header
        if (!file_.read(0, header_)) {
            tc::logError("TcvPlayer") << "File too small for a TCVC header";
            file_.close();
            return tc::LoadResult::fail(tc::LoadError::UnsupportedFormat, "truncated TCVC header");
        }

        // Validate signature
        if (std::memcmp(header_.signature, "TCVC", 4) != 0) {
            tc::logError("TcvPlayer") << "Invalid TCVC signature";
            file_.close();
            return tc::LoadResult::fail(tc::LoadError::UnsupportedFormat, "invalid TCVC signature");
        }

        // Validate version
        if (header_.version != TCV_VERSION) {
            tc::logError("TcvPlayer") << "Unsupported version: " << header_.version
                                      << " (expected " << TCV_VERSION << ")";
            file_.close();
            return tc::LoadResult::fail(tc::LoadError::UnsupportedFormat, "unsupported TCV version");
        }

        width_ = header_.width;
        height_ = header_.height;

        // Calculate block counts
        blocksX_ = (width_ + TCV_BLOCK_SIZE - 1) / TCV_BLOCK_SIZE;
        blocksY_ = (height_ + TCV_BLOCK_SIZE - 1) / TCV_BLOCK_SIZE;
        totalBlocks_ = blocksX_ * blocksY_;

        // Calculate BC7 data size per frame
        bc7FrameSize_ = totalBlocks_ * 256;  // 16 BC7 blocks per 16x16, 16 bytes each

        // Allocate BC7 buffer
        bc7Buffer_.resize(bc7FrameSize_);

        // Allocate the P-frame LZ4 output buffer (input is read from the mapping)
        lz4DecompressedBuffer_.resize(bc7FrameSize_ + 1024);

        // Allocate debug block types buffer
        debugBlockTypes_.resize(totalBlocks_, DebugBlockType::None);

        // Build frame index for seeking
        buildFrameIndex();

        // Decode workers (persistent; reused for every I-frame)
        startWorkers();

        // Decode first frame to get initial texture data
        // (必要: BC7圧縮テクスチャはImmutableなので初期データが必須)
        int paddedWidth = blocksX_ * TCV_BLOCK_SIZE;
        int paddedHeight = blocksY_ * TCV_BLOCK_SIZE;

        if (!frameIndex_.empty()) {
            // 最初のI-frameのデータを取得してテクスチャを作成
            const auto& firstFrameData = getIFrameData(0);
            if (firstFrameData.size() == bc7FrameSize_) {
                texture_.allocateCompressed(paddedWidth, paddedHeight,
                                            SG_PIXELFORMAT_BC7_RGBA,
                                            firstFrameData.data(), bc7FrameSize_);
            } else {
                tc::logError("TcvPlayer") << "Failed to decode first frame";
                return tc::LoadResult::fail(tc::LoadError::DecodeFailed, "failed to decode the first frame");
            }
        } else {
            tc::logError("TcvPlayer") << "No frames in file";
            return tc::LoadResult::fail(tc::LoadError::DecodeFailed, "no frames in file");
        }

        initialized_ = true;
        currentFrame_ = 0;  // 最初のフレームは既にデコード済み
        resetStats();

        // Load audio if present
        hasAudio_ = false;
        if (header_.audioCodec != 0 && header_.audioSize > 0) {
            loadAudio();
        }

        tc::logNotice("TcvPlayer") << "Loaded: " << width_ << "x" << height_
                                   << " @ " << header_.fps << " fps, "
                                   << header_.frameCount << " frames"
                                   << (hasAudio_ ? " (with audio)" : "");
        return tc::LoadResult::success();
    }

    void close() override {
        workers_.stop();  // also after a failed load (started before the first decode)
        if (!initialized_) return;

        // Stop audio
        if (hasAudio_) {
            audio_.stop();
        }

        file_.close();
        texture_.clear();
        frameIndex_.clear();
        iFrameCache_.clear();
        bc7Buffer_.clear();
        debugBlockTypes_.clear();

        initialized_ = false;
        playing_ = false;
        paused_ = false;
        frameNew_ = false;
        firstFrameReceived_ = false;
        done_ = false;
        currentFrame_ = -1;
        hasAudio_ = false;
    }

    // =========================================================================
    // Update
    // =========================================================================

    void update() override {
        if (!initialized_ || !playing_ || paused_) return;

        frameNew_ = false;

        // Advance playback time (can be negative for reverse)
        playbackTime_ += tc::getDeltaTime() * speed_;

        // Calculate target frame
        float duration = getDuration();
        int targetFrame = static_cast<int>((playbackTime_ / duration) * header_.frameCount);

        // Handle forward end (reached last frame)
        if (targetFrame >= static_cast<int>(header_.frameCount)) {
            if (loop_) {
                playbackTime_ = fmod(playbackTime_, duration);
                targetFrame = static_cast<int>((playbackTime_ / duration) * header_.frameCount);
            } else {
                targetFrame = header_.frameCount - 1;
                markDone();
                return;
            }
        }
        // Handle reverse end (reached first frame)
        else if (targetFrame < 0) {
            if (loop_) {
                playbackTime_ = duration + fmod(playbackTime_, duration);
                targetFrame = static_cast<int>((playbackTime_ / duration) * header_.frameCount);
                if (targetFrame >= static_cast<int>(header_.frameCount)) {
                    targetFrame = header_.frameCount - 1;
                }
            } else {
                targetFrame = 0;
                playbackTime_ = 0;
                markDone();
                return;
            }
        }

        if (targetFrame != currentFrame_) {
            decodeFrame(targetFrame);
            currentFrame_ = targetFrame;
            markFrameNew();
        }
    }

    // =========================================================================
    // Properties
    // =========================================================================

    float getDuration() const override {
        if (!initialized_ || header_.fps <= 0) return 0.0f;
        return static_cast<float>(header_.frameCount) / header_.fps;
    }

    float getFrameRate() const override {
        return initialized_ ? header_.fps : 0.0f;
    }

    float getPosition() const override {
        if (!initialized_ || header_.frameCount == 0) return 0.0f;
        return static_cast<float>(currentFrame_) / header_.frameCount;
    }

    // =========================================================================
    // Frame control
    // =========================================================================

    int getCurrentFrameImpl() const override {
        return currentFrame_ >= 0 ? currentFrame_ : 0;
    }

    int getTotalFramesImpl() const override {
        return initialized_ ? header_.frameCount : 0;
    }

    void setFrameImpl(int frame) override {
        if (!initialized_) return;
        frame = std::max(0, std::min(frame, static_cast<int>(header_.frameCount) - 1));
        if (frame != currentFrame_) {
            decodeFrame(frame);
            currentFrame_ = frame;
            markFrameNew();
        }
    }

    void nextFrameImpl() override {
        setFrameImpl(currentFrame_ + 1);
    }

    void previousFrameImpl() override {
        setFrameImpl(currentFrame_ - 1);
    }

    // =========================================================================
    // Pixel access (not available - TCV uses GPU-compressed textures)
    // =========================================================================

    unsigned char* getPixels() override { return nullptr; }
    const unsigned char* getPixels() const override { return nullptr; }

protected:
    void playImpl() override {
        playbackTime_ = 0;
        currentFrame_ = -1;

        // Start audio playback
        if (hasAudio_) {
            audio_.setPosition(0);
            audio_.play();
        }
    }

    void stopImpl() override {
        playbackTime_ = 0;
        currentFrame_ = -1;

        // Stop audio
        if (hasAudio_) {
            audio_.stop();
        }
    }

    void setPausedImpl(bool paused) override {
        if (hasAudio_) {
            if (paused) {
                audio_.pause();
            } else {
                audio_.resume();
            }
        }
    }

    void setPositionImpl(float pct) override {
        int frame = static_cast<int>(pct * header_.frameCount);
        setFrameImpl(frame);
        playbackTime_ = pct * getDuration();

        // Sync audio position
        if (hasAudio_) {
            audio_.setPosition(pct * audio_.getDuration());
        }
    }

    void setVolumeImpl(float vol) override {
        if (hasAudio_) {
            audio_.setVolume(vol);
        }
    }

    void setSpeedImpl(float speed) override {
        if (hasAudio_) {
            if (speed < 0) {
                // Mute audio during reverse playback
                audio_.setVolume(0);
            } else {
                audio_.setVolume(volume_);
                audio_.setSpeed(speed);
            }
        }
    }

    void setPanImpl(float pan) override {
        if (hasAudio_) {
            audio_.setPan(pan);
        }
    }

    void setLoopImpl(bool loop) override {
        if (hasAudio_) {
            audio_.setLoop(loop);
        }
    }

private:
    TcvMappedFile file_;
    TcvHeader header_ = {};

    int blocksX_ = 0;
    int blocksY_ = 0;
    int totalBlocks_ = 0;
    size_t bc7FrameSize_ = 0;

    std::vector<uint8_t> bc7Buffer_;
    int currentFrame_ = -1;
    double playbackTime_ = 0.0;

    // Audio playback
    bool hasAudio_ = false;
    tc::Sound audio_;

    // Frame index entry
    struct FrameIndexEntry {
        uint64_t offset;         // Packet start in the file
        uint8_t packetType;
        uint32_t refFrame;       // For P/REF frames
        uint32_t dataSize;       // Uncompressed size
        uint32_t compressedSize; // LZ4 compressed size
    };
    std::vector<FrameIndexEntry> frameIndex_;

    // I-frame cache: frameNumber -> BC7 data
    std::unordered_map<int, std::vector<uint8_t>> iFrameCache_;

    // P-frame LZ4 output (compressed input is read from the mapping)
    std::vector<char> lz4DecompressedBuffer_;

    // Debug mode
    bool debugMode_ = false;
    std::vector<DebugBlockType> debugBlockTypes_;

    // Performance stats (low-pass filtered)
    double decodeTimeMs_ = 0.0;

    // I-frame chunk decode threads (0 = auto)
    int decodeThreads_ = 0;
    TcvChunkWorkers workers_;

    void startWorkers() {
        int threads = decodeThreads_;
        if (threads <= 0) {
            // Auto: half the hardware threads, leaving the rest to the app. Use
            // setDecodeThreads() to pick a count for your footage and machine.
            int hw = static_cast<int>(std::thread::hardware_concurrency());
            threads = std::max(1, hw / 2);
        }
        workers_.start(threads - 1);  // the calling thread is the other decoder
    }

#ifdef TCV_PROFILE
    // Profiling (last frame)
    double profileFileIoMs_ = 0.0;
    double profileLz4Ms_ = 0.0;
    double profileCopyMs_ = 0.0;
    double profileGpuMs_ = 0.0;
    bool profileCacheHit_ = false;
    int profileChunkCount_ = 0;
#endif

    // Build index of frame offsets for seeking
    void buildFrameIndex() {
        frameIndex_.clear();
        frameIndex_.reserve(header_.frameCount);
        iFrameCache_.clear();

        uint64_t pos = header_.headerSize;

        for (uint32_t i = 0; i < header_.frameCount; i++) {
            FrameIndexEntry entry;
            entry.offset = pos;
            entry.compressedSize = 0;
            bool ok = file_.read(pos, entry.packetType);
            pos += 1;

            if (ok && entry.packetType == TCV_PACKET_I_FRAME) {
                // v5: I-frame chunked LZ4 [type][chunkCount][uncompSize][chunkSizes...][data...]
                uint8_t chunkCount = 0;
                ok = file_.read(pos, chunkCount) && file_.read(pos + 1, entry.dataSize);
                pos += 5;
                entry.refFrame = 0;

                // Sum chunk sizes to get total compressed size
                uint64_t totalCompressed = 0;
                for (int c = 0; ok && c < chunkCount; c++) {
                    uint32_t chunkSize = 0;
                    ok = file_.read(pos, chunkSize);
                    pos += 4;
                    totalCompressed += chunkSize;
                }
                entry.compressedSize = static_cast<uint32_t>(totalCompressed);
                pos += totalCompressed;
            } else if (ok && entry.packetType == TCV_PACKET_P_FRAME) {
                // v3: P-frame always LZ4 [type][refFrame][dataSize][compSize][data]
                ok = file_.read(pos, entry.refFrame) && file_.read(pos + 4, entry.dataSize) &&
                     file_.read(pos + 8, entry.compressedSize);
                pos += 12 + uint64_t(entry.compressedSize);
            } else if (ok && entry.packetType == TCV_PACKET_REF_FRAME) {
                ok = file_.read(pos, entry.refFrame);
                pos += 4;
                entry.dataSize = 0;
            } else if (ok) {
                tc::logWarning("TcvPlayer") << "Unknown packet type at frame " << i;
                break;
            }

            // A packet that runs past the end of the file (truncated copy) ends the index.
            if (!ok || pos > file_.size()) {
                tc::logWarning("TcvPlayer") << "Truncated file at frame " << i;
                break;
            }

            frameIndex_.push_back(entry);
        }

        tc::logNotice("TcvPlayer") << "Indexed " << frameIndex_.size() << " frames";
    }

    // Get or decode I-frame BC7 data (returns GPU layout BC7 buffer)
    const std::vector<uint8_t>& getIFrameData(int frameNum) {
        // Check cache
        auto it = iFrameCache_.find(frameNum);
        if (it != iFrameCache_.end()) {
#ifdef TCV_PROFILE
            profileCacheHit_ = true;
#endif
            return it->second;
        }
#ifdef TCV_PROFILE
        profileCacheHit_ = false;
#endif

        // Load from file
        const auto& entry = frameIndex_[frameNum];
        if (entry.packetType != TCV_PACKET_I_FRAME) {
            tc::logError("TcvPlayer") << "Frame " << frameNum << " is not an I-frame";
            static std::vector<uint8_t> empty;
            return empty;
        }

        // Result buffer (full GPU layout BC7 data). When the cache is full, reuse
        // the evicted entry's buffer: a fresh zero-filled 2 MB vector per I-frame
        // cost ~0.2 ms at 1080p, and footage that is mostly I-frames never hits
        // the cache anyway. resize() keeps the old bytes; LZ4 overwrites them all.
        std::vector<uint8_t> bc7Result;
        if (iFrameCache_.size() >= TCV_IFRAME_BUFFER_SIZE) {
            auto victim = iFrameCache_.begin();
            bc7Result = std::move(victim->second);
            iFrameCache_.erase(victim);
        }
        bc7Result.resize(bc7FrameSize_);

#ifdef TCV_PROFILE
        auto ioStart = std::chrono::high_resolution_clock::now();
#endif

        // Read packet header (v5 chunked format): [type][chunkCount][uncompSize][chunkSizes...][data...]
        uint8_t chunkCount = 0;
        uint32_t uncompressedSize = 0;
        uint64_t pos = entry.offset + 1;
        bool headerOk = file_.read(pos, chunkCount) && file_.read(pos + 1, uncompressedSize);
        pos += 5;

        // Read chunk sizes
        std::vector<uint32_t> chunkSizes(chunkCount);
        uint64_t totalCompressed = 0;
        for (int i = 0; headerOk && i < chunkCount; i++) {
            headerOk = file_.read(pos, chunkSizes[i]);
            pos += 4;
            totalCompressed += chunkSizes[i];
        }

        // The chunks are decompressed straight from the mapping.
        const uint8_t* compressed = headerOk ? file_.at(pos, totalCompressed) : nullptr;
        if (!compressed || chunkCount == 0 || uncompressedSize != bc7FrameSize_) {
            tc::logError("TcvPlayer") << "Corrupt I-frame packet " << frameNum;
            static std::vector<uint8_t> empty;
            return empty;
        }

#ifdef TCV_PROFILE
        auto ioEnd = std::chrono::high_resolution_clock::now();
        profileFileIoMs_ = std::chrono::duration<double, std::milli>(ioEnd - ioStart).count();
        auto lz4Start = std::chrono::high_resolution_clock::now();
#endif

        // LZ4 decompress the chunks in parallel on the player's persistent workers,
        // each straight into its region of bc7Result.
        size_t chunkUncompressedSize = uncompressedSize / chunkCount;
        std::vector<size_t> inOffsets(chunkCount);
        size_t compressedOffset = 0;
        for (int i = 0; i < chunkCount; i++) {
            inOffsets[i] = compressedOffset;
            compressedOffset += chunkSizes[i];
        }
        std::vector<uint8_t> ok(chunkCount, 1);  // not vector<bool>: written from several threads
        auto decodeChunk = [&](int i) {
            size_t outOffset = i * chunkUncompressedSize;
            size_t outSize = (i == chunkCount - 1)
                ? (uncompressedSize - outOffset)  // Last chunk gets remainder
                : chunkUncompressedSize;
            int decompressed = tc::decompress(
                compressed + inOffsets[i], chunkSizes[i],
                bc7Result.data() + outOffset, outSize, tc::Codec::LZ4);
            if (decompressed != static_cast<int>(outSize)) ok[i] = 0;
        };
        workers_.run(chunkCount, decodeChunk);

        for (int i = 0; i < chunkCount; i++) {
            if (!ok[i]) {
                tc::logError("TcvPlayer") << "LZ4 decompression failed for I-frame " << frameNum << " chunk " << i;
                static std::vector<uint8_t> empty;
                return empty;
            }
        }

#ifdef TCV_PROFILE
        auto lz4End = std::chrono::high_resolution_clock::now();
        profileLz4Ms_ = std::chrono::duration<double, std::milli>(lz4End - lz4Start).count();
        profileCopyMs_ = 0.0;
        profileChunkCount_ = chunkCount;
#endif

        // Cache it (the size limit was enforced above, before the decode)
        auto& cached = iFrameCache_[frameNum];
        cached = std::move(bc7Result);
        return cached;
    }

    // Decode I-frame BC7 blocks from buffer (v4: already GPU layout, just memcpy)
    void decodeIFrameFromBuffer(const uint8_t* data, uint8_t* bc7Output) {
        // v4: Data is already in GPU layout (4x4 blocks row-major)
        std::memcpy(bc7Output, data, bc7FrameSize_);
    }

    // Copy a 16x16 block (256 bytes) directly to GPU buffer position
    // Optimized: copy 4 BC7 blocks (64 bytes) per row instead of 16 individual copies
    void copyBlockToGpuPosition(int bx16, int by16, const uint8_t* bc7Data) {
        int bc7BlocksX = blocksX_ * 4;  // 4x4 blocks per row

        for (int by4 = 0; by4 < 4; by4++) {
            int gpuY = by16 * 4 + by4;
            int gpuX = bx16 * 4;
            int gpuIdx = gpuY * bc7BlocksX + gpuX;

            // Copy 4 BC7 blocks (64 bytes) at once - they're contiguous in both src and dst
            std::memcpy(bc7Buffer_.data() + gpuIdx * 16,
                       bc7Data + by4 * 64,
                       64);
        }
    }

    // Decode a specific frame
    void decodeFrame(int frameNum) {
        auto startTime = std::chrono::high_resolution_clock::now();

        if (frameNum < 0 || frameNum >= static_cast<int>(frameIndex_.size())) {
            return;
        }

        const auto& entry = frameIndex_[frameNum];

        // Clear debug block types (None = no overlay for I-frames)
        if (debugMode_) {
            std::fill(debugBlockTypes_.begin(), debugBlockTypes_.end(), DebugBlockType::None);
        }

        if (entry.packetType == TCV_PACKET_I_FRAME) {
            // I-frame: upload directly from cache (no intermediate copy)
            const auto& iData = getIFrameData(frameNum);
            if (iData.size() == bc7FrameSize_) {
#ifdef TCV_PROFILE
                auto gpuStart = std::chrono::high_resolution_clock::now();
#endif
                texture_.updateCompressed(iData.data(), bc7FrameSize_);
#ifdef TCV_PROFILE
                auto gpuEnd = std::chrono::high_resolution_clock::now();
                profileGpuMs_ = std::chrono::duration<double, std::milli>(gpuEnd - gpuStart).count();
#endif
                // For debug: I-frame = all BC7
                if (debugMode_) {
                    std::fill(debugBlockTypes_.begin(), debugBlockTypes_.end(), DebugBlockType::BC7);
                }

                // Record decode time (low-pass filter) and return early
                auto endTime = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
                if (decodeTimeMs_ == 0.0) {
                    decodeTimeMs_ = ms;
                } else {
                    constexpr double kAlpha = 0.05;
                    decodeTimeMs_ = decodeTimeMs_ * (1.0 - kAlpha) + ms * kAlpha;
                }

#ifdef TCV_PROFILE
                // Log every I-frame profile
                tc::logNotice("TcvPlayer") << "I-frame " << frameNum
                    << ": IO=" << profileFileIoMs_
                    << "ms, LZ4=" << profileLz4Ms_
                    << "ms, GPU=" << profileGpuMs_
                    << "ms, Chunks=" << profileChunkCount_
                    << ", Cache=" << (profileCacheHit_ ? "HIT" : "MISS")
                    << ", Total=" << ms << "ms";
#endif
                return;
            }
        } else if (entry.packetType == TCV_PACKET_REF_FRAME) {
            // REF-frame: upload directly from referenced I-frame cache
            const auto& refData = getIFrameData(entry.refFrame);
            if (refData.size() == bc7FrameSize_) {
                texture_.updateCompressed(refData.data(), bc7FrameSize_);
                // For debug: REF-frame = all Skip
                if (debugMode_) {
                    std::fill(debugBlockTypes_.begin(), debugBlockTypes_.end(), DebugBlockType::Skip);
                }
                // Record decode time (low-pass filter) and return early
                auto endTime = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
                if (decodeTimeMs_ == 0.0) {
                    decodeTimeMs_ = ms;
                } else {
                    constexpr double kAlpha = 0.05;
                    decodeTimeMs_ = decodeTimeMs_ * (1.0 - kAlpha) + ms * kAlpha;
                }
                return;
            }
        } else if (entry.packetType == TCV_PACKET_P_FRAME) {
            // P-frame: start with reference (GPU layout), apply deltas
            const auto& refData = getIFrameData(entry.refFrame);
            if (refData.size() == bc7FrameSize_) {
                std::memcpy(bc7Buffer_.data(), refData.data(), bc7FrameSize_);
            }

            // Decompress block commands straight from the mapping (v3: always LZ4)
            // [type][refFrame][dataSize][compSize][data], sizes already indexed
            const uint8_t* compressed = file_.at(entry.offset + 13, entry.compressedSize);
            int decompressed = compressed ? tc::decompress(
                compressed, entry.compressedSize,
                lz4DecompressedBuffer_.data(), lz4DecompressedBuffer_.size(),
                tc::Codec::LZ4) : -1;
            if (decompressed != static_cast<int>(entry.dataSize)) {
                tc::logError("TcvPlayer") << "LZ4 decompression failed for P-frame " << frameNum;
                return;
            }

            const uint8_t* blockData = reinterpret_cast<const uint8_t*>(lz4DecompressedBuffer_.data());

            // Parse block commands (v3: SKIP or BC7 only)
            size_t offset = 0;
            int blockIdx = 0;
            while (blockIdx < totalBlocks_) {
                uint8_t cmd = blockData[offset++];

                uint8_t blockType = cmd & TCV_BLOCK_TYPE_MASK;
                int runLength = (cmd & TCV_BLOCK_RUN_MASK) + 1;

                for (int i = 0; i < runLength && blockIdx < totalBlocks_; i++, blockIdx++) {
                    int bx16 = blockIdx % blocksX_;
                    int by16 = blockIdx / blocksX_;

                    if (blockType == TCV_BLOCK_SKIP) {
                        // Keep reference data (already copied in GPU layout)
                        if (debugMode_) debugBlockTypes_[blockIdx] = DebugBlockType::Skip;
                    } else {  // TCV_BLOCK_BC7
                        copyBlockToGpuPosition(bx16, by16, blockData + offset);
                        offset += 256;
                        if (debugMode_) debugBlockTypes_[blockIdx] = DebugBlockType::BC7;
                    }
                }
            }
        }

        // Upload to GPU texture
        auto gpuStart = std::chrono::high_resolution_clock::now();
        texture_.updateCompressed(bc7Buffer_.data(), bc7FrameSize_);
        auto gpuEnd = std::chrono::high_resolution_clock::now();
        double gpuMs = std::chrono::duration<double, std::milli>(gpuEnd - gpuStart).count();

        // Record decode time (low-pass filter)
        auto endTime = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        if (decodeTimeMs_ == 0.0) {
            decodeTimeMs_ = ms;
        } else {
            constexpr double kAlpha = 0.05;
            decodeTimeMs_ = decodeTimeMs_ * (1.0 - kAlpha) + ms * kAlpha;
        }

        // Debug: log P-frame timing every 30 frames
        static int pFrameLogCount = 0;
        if (pFrameLogCount++ % 30 == 0) {
            tc::logNotice("TcvPlayer") << "P-frame " << frameNum
                << ": total=" << ms << "ms, GPU=" << gpuMs << "ms";
        }
    }

    // Load audio from TCV file
    void loadAudio() {
        if (header_.audioOffset == 0 || header_.audioSize == 0) {
            return;
        }

        // Read audio data from the mapping
        const uint8_t* audioBytes = file_.at(header_.audioOffset, header_.audioSize);
        if (!audioBytes) {
            tc::logWarning("TcvPlayer") << "Audio data runs past the end of the file";
            return;
        }
        std::vector<uint8_t> audioData(audioBytes, audioBytes + header_.audioSize);

        // Create SoundBuffer and decode based on codec
        auto soundBuffer = std::make_shared<tc::SoundBuffer>();
        bool loaded = false;

        // Check codec FourCC (big-endian: MSB is first character)
        uint32_t codec = header_.audioCodec;
        char codecStr[5] = {0};
        codecStr[0] = static_cast<char>((codec >> 24) & 0xFF);
        codecStr[1] = static_cast<char>((codec >> 16) & 0xFF);
        codecStr[2] = static_cast<char>((codec >> 8) & 0xFF);
        codecStr[3] = static_cast<char>(codec & 0xFF);

        // FourCC values (big-endian)
        // kAudioFormatMPEGLayer3 = '.mp3' = 0x2E6D7033
        constexpr uint32_t FOURCC_MP3_DOT = 0x2E6D7033;  // '.mp3' (kAudioFormatMPEGLayer3)
        constexpr uint32_t FOURCC_MP3_SPC = 0x6D703320;  // 'mp3 '
        constexpr uint32_t FOURCC_AAC     = 0x61616320;  // 'aac ' (kAudioFormatMPEG4AAC)
        constexpr uint32_t FOURCC_MP4A    = 0x6D703461;  // 'mp4a'
        constexpr uint32_t FOURCC_AAC_MF  = 0x1610;      // WAVE_FORMAT_MPEG_HEAAC (Windows Media Foundation)
        constexpr uint32_t FOURCC_LPCM    = 0x6C70636D;  // 'lpcm'
        constexpr uint32_t FOURCC_SOWT    = 0x736F7774;  // 'sowt' (16-bit little-endian PCM)
        constexpr uint32_t FOURCC_TWOS    = 0x74776F73;  // 'twos' (16-bit big-endian PCM)

        // Debug: show codec value
        tc::logNotice("TcvPlayer") << "Audio codec: " << codecStr
                                   << " (0x" << std::hex << codec << std::dec << ")"
                                   << ", data size: " << audioData.size() << " bytes";

        if (codec == FOURCC_MP3_DOT || codec == FOURCC_MP3_SPC) {
            // MP3: decode from memory
            tc::logNotice("TcvPlayer") << "Attempting MP3 decode...";
            loaded = soundBuffer->loadMp3FromMemory(audioData.data(), audioData.size()).ok();
            if (loaded) {
                tc::logNotice("TcvPlayer") << "MP3 decode successful: "
                                           << soundBuffer->channels << " ch, "
                                           << soundBuffer->sampleRate << " Hz, "
                                           << soundBuffer->numSamples << " samples";
            } else {
                tc::logError("TcvPlayer") << "MP3 decode failed!";
            }
        } else if (codec == FOURCC_AAC || codec == FOURCC_MP4A || codec == FOURCC_AAC_MF) {
            // AAC: decode using platform-specific decoder
            tc::logNotice("TcvPlayer") << "Attempting AAC decode...";
            loaded = soundBuffer->loadAacFromMemory(audioData.data(), audioData.size()).ok();
            if (loaded) {
                tc::logNotice("TcvPlayer") << "AAC decode successful: "
                                           << soundBuffer->channels << " ch, "
                                           << soundBuffer->sampleRate << " Hz, "
                                           << soundBuffer->numSamples << " samples";
            } else {
                tc::logWarning("TcvPlayer") << "AAC decode failed (may not be supported on this platform)";
            }
        } else if (codec == FOURCC_LPCM || codec == FOURCC_SOWT || codec == FOURCC_TWOS) {
            // PCM: load raw audio data directly
            tc::logNotice("TcvPlayer") << "Loading PCM audio...";

            int sampleRate = static_cast<int>(header_.audioSampleRate);
            int channels = static_cast<int>(header_.audioChannels);

            if (sampleRate > 0 && channels > 0) {
                // Assume 16-bit PCM (2 bytes per sample per channel)
                size_t bytesPerSample = 2 * channels;
                size_t numSamples = audioData.size() / bytesPerSample;

                soundBuffer->sampleRate = sampleRate;
                soundBuffer->channels = channels;
                soundBuffer->numSamples = static_cast<int>(numSamples);
                soundBuffer->samples.resize(numSamples * channels);

                const int16_t* src = reinterpret_cast<const int16_t*>(audioData.data());
                bool bigEndian = (codec == FOURCC_TWOS);

                for (size_t i = 0; i < numSamples * channels; i++) {
                    int16_t sample = src[i];
                    if (bigEndian) {
                        // Swap bytes for big-endian
                        sample = static_cast<int16_t>(((sample & 0xFF) << 8) | ((sample >> 8) & 0xFF));
                    }
                    soundBuffer->samples[i] = sample / 32768.0f;
                }

                loaded = true;
                tc::logNotice("TcvPlayer") << "PCM load successful: "
                                           << channels << " ch, "
                                           << sampleRate << " Hz, "
                                           << numSamples << " samples";
            } else {
                tc::logWarning("TcvPlayer") << "PCM audio missing sample rate or channel info in header";
            }
        } else {
            tc::logWarning("TcvPlayer") << "Unknown audio codec - cannot decode";
        }

        if (loaded) {
            audio_.loadFromBuffer(soundBuffer);
            hasAudio_ = true;
            tc::logNotice("TcvPlayer") << "Audio loaded: " << audioData.size() << " bytes, "
                                       << soundBuffer->getDuration() << "s duration";
        }
    }
};

} // namespace tcx::tcv

// -----------------------------------------------------------------------------
// Backward compatibility. The canonical namespace is now `tcx::tcv`. These
// silent aliases keep older code compiling: flat `tcx::TcvPlayer` and legacy
// `tc::TcvPlayer` / `trussc::TcvPlayer`. DEPRECATED — removed in v1.0.0.
// (No [[deprecated]] attribute: under the usual `using namespace tc;` it would
//  warn on idiomatic unqualified use too. See tcxTcv README for migration.)
// -----------------------------------------------------------------------------
namespace tcx    { using tcv::TcvPlayer; } // deprecated: remove at v1.0.0
namespace trussc { using tcx::tcv::TcvPlayer; } // deprecated: remove at v1.0.0
