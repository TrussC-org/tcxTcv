// =============================================================================
// tcxTcv tests - headless behavioral test (no window).
//
// Built and run by CI (TrussC-org/ci-actions build-addon.yml): exit 0 = all
// pass, non-zero = failure -> CI fails. Console only, so it runs on headless
// macOS / Windows / Linux runners.
//
// Decoding a .tcv is GPU-bound (BC7 blocks are uploaded straight to a texture;
// TcvPlayer::getPixels() returns nullptr), so there is no CPU pixel read-back
// to round-trip against. These tests therefore verify the *encoder* and the
// on-disk TCVC container: header fields, packet framing, and the I / P / REF
// frame-type decisions. Files go to the OS temp dir.
// =============================================================================

#include <tcxTcv.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace std;
using namespace tcx;

static int g_pass = 0, g_fail = 0;
static void check(const char* name, bool ok) {
    printf("%-58s %s\n", name, ok ? "PASS" : "FAIL");
    fflush(stdout);
    ok ? ++g_pass : ++g_fail;
}

static string tmp(const char* name) {
    return (filesystem::temp_directory_path() / name).string();
}

// TCVC container constants (mirror tcTcvEncoder.h, kept local so the test pins
// the on-disk format rather than tracking whatever the header currently says).
static constexpr int      HEADER_SIZE = 64;
static constexpr uint16_t VERSION     = 5;
static constexpr uint16_t BLOCK_SIZE  = 16;
static constexpr uint8_t  PKT_I       = 0x01;
static constexpr uint8_t  PKT_P       = 0x02;
static constexpr uint8_t  PKT_REF     = 0x03;

// ---- little-endian readers over a byte buffer ----
static uint16_t rd16(const vector<uint8_t>& b, size_t p) {
    return uint16_t(b[p]) | (uint16_t(b[p + 1]) << 8);
}
static uint32_t rd32(const vector<uint8_t>& b, size_t p) {
    return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) |
           (uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24);
}
static float rdF32(const vector<uint8_t>& b, size_t p) {
    uint32_t u = rd32(b, p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// On-disk length of the I-frame packet starting at offset p:
//   [type:1][chunkCount:1][dataSize:4][chunkSize:4 * chunkCount][chunk data...]
static size_t iFrameLen(const vector<uint8_t>& b, size_t p) {
    int chunkCount = b[p + 1];
    size_t len = 6 + size_t(chunkCount) * 4;
    for (int i = 0; i < chunkCount; ++i) len += rd32(b, p + 6 + size_t(i) * 4);
    return len;
}

static vector<uint8_t> readFile(const string& path) {
    ifstream f(path, ios::binary);
    return vector<uint8_t>((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
}

// Solid-color RGBA frame (w*h*4). Color makes consecutive frames differ.
static vector<uint8_t> solidFrame(int w, int h, uint8_t r, uint8_t g, uint8_t bl) {
    vector<uint8_t> px(size_t(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r; px[i + 1] = g; px[i + 2] = bl; px[i + 3] = 255;
    }
    return px;
}

int main() {
    printf("tcxTcv tests\n\n");

    // ----- 1. Header is a well-formed 64-byte TCVC header -----
    {
        const int w = 64, h = 48;
        const float fps = 30.0f;
        string path = tmp("tcxtcv_header.tcv");

        TcvEncoder enc;
        check("header: begin() ok", enc.begin(path, w, h, fps));
        enc.addFrame(solidFrame(w, h, 200, 30, 30).data());
        enc.addFrame(solidFrame(w, h, 30, 200, 30).data());
        enc.addFrame(solidFrame(w, h, 30, 30, 200).data());
        check("header: end() ok", enc.end());
        check("header: getFrameCount()==3", enc.getFrameCount() == 3);

        auto b = readFile(path);
        check("header: file >= 64 bytes + packets", b.size() > HEADER_SIZE);
        check("header: signature 'TCVC'",
              b.size() >= 4 && b[0] == 'T' && b[1] == 'C' && b[2] == 'V' && b[3] == 'C');
        check("header: version == 5", rd16(b, 0x04) == VERSION);
        check("header: headerSize == 64", rd16(b, 0x06) == HEADER_SIZE);
        check("header: width round-trips", rd32(b, 0x08) == uint32_t(w));
        check("header: height round-trips", rd32(b, 0x0C) == uint32_t(h));
        check("header: frameCount written by end() == 3", rd32(b, 0x10) == 3u);
        check("header: fps round-trips", rdF32(b, 0x14) == fps);
        check("header: blockSize == 16", rd16(b, 0x18) == BLOCK_SIZE);
        check("header: first packet is an I-frame", b[HEADER_SIZE] == PKT_I);
    }

    // ----- 2. Encoder error paths -----
    {
        string path = tmp("tcxtcv_err.tcv");
        TcvEncoder enc;
        check("error: begin() rejects zero dimensions", !enc.begin(path, 0, 0, 30.0f));

        TcvEncoder enc2;
        check("error: addFrame() before begin() fails",
              !enc2.addFrame(solidFrame(16, 16, 1, 2, 3).data()));

        TcvEncoder enc3;
        check("error: begin() ok", enc3.begin(tmp("tcxtcv_err2.tcv"), 32, 32, 30.0f));
        check("error: second begin() while encoding fails",
              !enc3.begin(tmp("tcxtcv_err3.tcv"), 32, 32, 30.0f));
        enc3.end();
    }

    // ----- 3. Force-all-I-frames: every packet is an I-frame, stream is tight -----
    {
        const int w = 32, h = 32, n = 4;
        string path = tmp("tcxtcv_alli.tcv");

        TcvEncoder enc;
        enc.setForceAllIFrames(true);
        enc.begin(path, w, h, 30.0f);
        for (int i = 0; i < n; ++i)
            enc.addFrame(solidFrame(w, h, uint8_t(40 * i), uint8_t(10 * i), 60).data());
        enc.end();

        auto b = readFile(path);
        size_t p = HEADER_SIZE;
        int seen = 0;
        bool allI = true, walked = true;
        for (int i = 0; i < n; ++i) {
            if (p >= b.size()) { walked = false; break; }
            if (b[p] != PKT_I) allI = false;
            p += iFrameLen(b, p);
            ++seen;
        }
        check("all-I: walked all 4 packets", walked && seen == n);
        check("all-I: every packet is an I-frame", allI);
        check("all-I: packet stream ends exactly at EOF", p == b.size());
    }

    // ----- 4. An exact duplicate frame becomes a REF-frame -----
    {
        const int w = 48, h = 48;
        string path = tmp("tcxtcv_ref.tcv");

        auto frame = solidFrame(w, h, 123, 77, 200);
        TcvEncoder enc;
        enc.begin(path, w, h, 30.0f);
        enc.addFrame(frame.data());   // frame 0 -> I
        enc.addFrame(frame.data());   // frame 1 -> exact dup -> REF
        enc.end();

        auto b = readFile(path);
        check("ref: packet 0 is an I-frame", b[HEADER_SIZE] == PKT_I);
        size_t p1 = HEADER_SIZE + iFrameLen(b, HEADER_SIZE);
        check("ref: duplicate frame 1 is a REF-frame", p1 < b.size() && b[p1] == PKT_REF);
    }

    // ----- 5. A partially-changed frame becomes a P-frame (delta), not I or REF -----
    {
        const int w = 64, h = 64;
        string path = tmp("tcxtcv_pframe.tcv");

        auto base = solidFrame(w, h, 90, 140, 60);
        auto changed = base;
        // Recolor the top-left 16x16 block only -> some blocks differ, most skip.
        for (int y = 0; y < BLOCK_SIZE; ++y)
            for (int x = 0; x < BLOCK_SIZE; ++x) {
                size_t i = (size_t(y) * w + x) * 4;
                changed[i] = 255; changed[i + 1] = 0; changed[i + 2] = 0;
            }

        TcvEncoder enc;
        enc.begin(path, w, h, 30.0f);
        enc.addFrame(base.data());      // frame 0 -> I
        enc.addFrame(changed.data());   // frame 1 -> partial change -> P
        enc.end();

        auto b = readFile(path);
        check("pframe: packet 0 is an I-frame", b[HEADER_SIZE] == PKT_I);
        size_t p1 = HEADER_SIZE + iFrameLen(b, HEADER_SIZE);
        check("pframe: changed frame 1 is a P-frame", p1 < b.size() && b[p1] == PKT_P);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
