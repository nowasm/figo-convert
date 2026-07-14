// Shared PNG encoder for the engine exporters (figo2godot / figo2cocos /
// figo2unity). RGBA8, self-contained, deterministic.
//
// Pipeline: Paeth-filter every row (flat areas become zero runs, gradients
// become small deltas), then a real DEFLATE stream — fixed-Huffman block with
// greedy hash-chain LZ77 (window 32 KB, match 3..258, capped chain walk).
// Typically 5-20x smaller than the old stored-zlib output on UI art while
// staying dependency-free and fast enough for hundreds of sprites.
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace figopng {

inline uint32_t crcByte(uint32_t n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    return c;
}
inline uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) table[i] = crcByte(i);
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}
inline void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back(x & 0xff);
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32be(out, static_cast<uint32_t>(data.size()));
    size_t typeStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32(out.data() + typeStart, 4 + data.size());
    put32be(out, crc ^ 0xFFFFFFFFu);
}

// LSB-first deflate bit stream; Huffman codes go in MSB-of-code first.
struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int nbits = 0;
    void put(uint32_t bits, int n) {
        acc |= bits << nbits;
        nbits += n;
        while (nbits >= 8) {
            out.push_back(acc & 0xff);
            acc >>= 8;
            nbits -= 8;
        }
    }
    void putHuff(uint32_t code, int n) {  // reverse: codes pack MSB-first
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) r = (r << 1) | ((code >> i) & 1);
        put(r, n);
    }
    void flush() {
        if (nbits > 0) {
            out.push_back(acc & 0xff);
            acc = 0;
            nbits = 0;
        }
    }
};

inline void fixedLit(BitWriter& bw, int v) {  // RFC1951 fixed lit/len codes
    if (v <= 143) bw.putHuff(0x30 + v, 8);
    else if (v <= 255) bw.putHuff(0x190 + (v - 144), 9);
    else if (v <= 279) bw.putHuff(v - 256, 7);
    else bw.putHuff(0xC0 + (v - 280), 8);
}

// One fixed-Huffman deflate block over `in`, greedy hash-chain LZ77.
inline void deflateFixed(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    static const struct { uint16_t base; uint8_t extra; } kLen[] = {
        {3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},{11,1},{13,1},{15,1},{17,1},
        {19,2},{23,2},{27,2},{31,2},{35,3},{43,3},{51,3},{59,3},{67,4},{83,4},{99,4},
        {115,4},{131,5},{163,5},{195,5},{227,5},{258,0}};
    static const struct { uint32_t base; uint8_t extra; } kDist[] = {
        {1,0},{2,0},{3,0},{4,0},{5,1},{7,1},{9,2},{13,2},{17,3},{25,3},{33,4},{49,4},
        {65,5},{97,5},{129,6},{193,6},{257,7},{385,7},{513,8},{769,8},{1025,9},{1537,9},
        {2049,10},{3073,10},{4097,11},{6145,11},{8193,12},{12289,12},{16385,13},{24577,13}};

    BitWriter bw{out};
    bw.put(1, 1);  // final block
    bw.put(1, 2);  // fixed Huffman

    const size_t n = in.size();
    constexpr int kHashBits = 15;
    constexpr int kHashSize = 1 << kHashBits;
    constexpr int kMaxChain = 64;
    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(n, -1);
    auto hash3 = [&](size_t i) {
        return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & (kHashSize - 1);
    };
    auto insert = [&](size_t i) {
        const int h = hash3(i);
        prev[i] = head[h];
        head[h] = static_cast<int32_t>(i);
    };

    size_t i = 0;
    while (i < n) {
        size_t bestLen = 0, bestDist = 0;
        if (i + 3 <= n) {
            const size_t maxLen = std::min<size_t>(258, n - i);
            int32_t j = head[hash3(i)];
            int chain = kMaxChain;
            while (j >= 0 && chain-- > 0 && i - j <= 32768) {
                size_t l = 0;
                while (l < maxLen && in[j + l] == in[i + l]) ++l;
                if (l > bestLen) {
                    bestLen = l;
                    bestDist = i - j;
                    if (l >= maxLen) break;
                }
                j = prev[j];
            }
            insert(i);
        }
        if (bestLen >= 3) {
            int lc = 0;
            while (lc < 28 && kLen[lc + 1].base <= bestLen) ++lc;
            fixedLit(bw, 257 + lc);
            if (kLen[lc].extra) bw.put(static_cast<uint32_t>(bestLen - kLen[lc].base), kLen[lc].extra);
            int dc = 0;
            while (dc < 29 && kDist[dc + 1].base <= bestDist) ++dc;
            bw.putHuff(dc, 5);
            if (kDist[dc].extra) bw.put(static_cast<uint32_t>(bestDist - kDist[dc].base), kDist[dc].extra);
            for (size_t k = i + 1; k < i + bestLen && k + 3 <= n; ++k) insert(k);
            i += bestLen;
        } else {
            fixedLit(bw, in[i]);
            ++i;
        }
    }
    fixedLit(bw, 256);  // end of block
    bw.flush();
}

inline uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    return pb <= pc ? static_cast<uint8_t>(b) : static_cast<uint8_t>(c);
}

// rgba: row-major uint32, memory byte order R,G,B,A (ThorVG ABGR8888S).
inline bool writePng(const std::filesystem::path& path, const uint32_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    const size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> cur(stride), up(stride, 0);
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + stride));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t v = rgba[static_cast<size_t>(y) * w + x];
            cur[x * 4 + 0] = v & 0xff;          // R
            cur[x * 4 + 1] = (v >> 8) & 0xff;   // G
            cur[x * 4 + 2] = (v >> 16) & 0xff;  // B
            cur[x * 4 + 3] = (v >> 24) & 0xff;  // A
        }
        raw.push_back(4);  // Paeth filter
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= 4 ? cur[x - 4] : 0;
            const int b = up[x];
            const int c = x >= 4 ? up[x - 4] : 0;
            raw.push_back(static_cast<uint8_t>(cur[x] - paeth(a, b, c)));
        }
        std::swap(cur, up);
    }

    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    deflateFixed(raw, z);
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32be(ihdr, w);
    put32be(ihdr, h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), out.size());
    return static_cast<bool>(f);
}

}  // namespace figopng
