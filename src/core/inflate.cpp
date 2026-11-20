#include "inflate.h"

#include <cstring>

namespace adb {

namespace {

// ---------------------------------------------------------------- 位流
struct Bits {
    const uint8_t* data = nullptr;
    size_t size = 0;      // 字节数
    size_t pos = 0;       // 已取走的字节
    uint32_t buf = 0;     // 位缓冲
    int count = 0;        // 缓冲里还有多少位
    bool overrun = false; // 越过了流末尾（只是记账，不立刻判错）

    // 往缓冲里续字节；越界时补 0 并记账
    void Refill() {
        while (count <= 24) {
            uint32_t b = 0;
            if (pos < size) b = data[pos];
            else overrun = true;
            ++pos;
            buf |= b << count;
            count += 8;
        }
    }

    // 取 n 位（n <= 24）。真读不到才置 bad。
    uint32_t Get(int n) {
        if (n == 0) return 0;
        if (count < n) Refill();
        uint32_t v = buf & ((1u << n) - 1u);
        buf >>= n;
        count -= n;
        return v;
    }

    // 允许越过末尾几个字节：deflate 最后一个块之后本来就可能多读几位，
    // 真正的正确性由"输出长度 == zip 里的 uncompressed size"来把关。
    bool Bad() const { return pos > size + 8; }

    // 丢弃若干位，对齐到字节
    void AlignToByte() {
        int drop = count % 8;
        buf >>= drop;
        count -= drop;
    }
};

// ---------------------------------------------------------------- Huffman
struct Huffman {
    // 用"计数 + 符号表"的规范 Huffman 解码，最大码长 15
    uint16_t counts[16] = {0};
    uint16_t symbols[288] = {0};

    bool Build(const uint8_t* lengths, int n) {
        std::memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        if (counts[0] == n) return false;      // 全 0 长度：这棵树没人用

        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) return false;        // 码长集合非法
        }

        uint16_t offs[16] = {0};
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + counts[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        return true;
    }

    // 从位流里解一个符号；失败返回 -1
    int Decode(Bits* bits) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(bits->Get(1));
            if (bits->Bad()) return -1;
            int count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr int kMaxCodes = 288;
constexpr int kMaxLit = 288;
constexpr int kMaxDist = 30;

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t  kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                8193, 12289, 16385, 24577};
const uint8_t  kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

const uint8_t kLenOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// 解码一棵动态 Huffman 树，返回读入的符号数（0 = 失败）
bool ReadDynamicTrees(Bits* bits, Huffman* lit, Huffman* dist) {
    const int hlit = static_cast<int>(bits->Get(5)) + 257;
    const int hdist = static_cast<int>(bits->Get(5)) + 1;
    const int hclen = static_cast<int>(bits->Get(4)) + 4;
    if (bits->Bad() || hlit > kMaxLit || hdist > kMaxDist) return false;

    uint8_t clen[19] = {0};
    for (int i = 0; i < hclen; ++i) clen[kLenOrder[i]] = static_cast<uint8_t>(bits->Get(3));
    if (bits->Bad()) return false;

    Huffman clTree;
    if (!clTree.Build(clen, 19)) return false;

    uint8_t lengths[kMaxLit + kMaxDist] = {0};
    int n = 0;
    while (n < hlit + hdist) {
        int sym = clTree.Decode(bits);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[n++] = static_cast<uint8_t>(sym);
            continue;
        }
        int repeat = 0;
        uint8_t value = 0;
        if (sym == 16) {
            if (n == 0) return false;
            value = lengths[n - 1];
            repeat = 3 + static_cast<int>(bits->Get(2));
        } else if (sym == 17) {
            repeat = 3 + static_cast<int>(bits->Get(3));
        } else {
            repeat = 11 + static_cast<int>(bits->Get(7));
        }
        if (bits->Bad() || n + repeat > hlit + hdist) return false;
        while (repeat-- > 0) lengths[n++] = value;
    }

    if (!lit->Build(lengths, hlit)) return false;
    if (!dist->Build(lengths + hlit, hdist)) {
        // 只有"没有距离码"这一种情况允许：此时不该出现长度/距离对
        bool allZero = true;
        for (int i = 0; i < hdist; ++i) if (lengths[hlit + i]) { allZero = false; break; }
        if (!allZero) return false;
    }
    return true;
}

}  // namespace

bool InflateRaw(const uint8_t* in, size_t inSize, size_t expectedSize, std::vector<uint8_t>* out) {
    if (!out || !in) return false;
    out->clear();
    out->reserve(expectedSize ? expectedSize : (inSize * 3));

    Bits bits;
    bits.data = in;
    bits.size = inSize;

    bool last = false;
    while (!last) {
        last = bits.Get(1) != 0;
        const int type = static_cast<int>(bits.Get(2));
        if (bits.Bad()) return false;

        if (type == 0) {
            // 存储块：先对齐到字节，再读 LEN / NLEN
            bits.AlignToByte();
            bits.Get(bits.count % 8);            // 清掉残留位
            uint32_t len = bits.Get(16);
            uint32_t nlen = bits.Get(16);
            if (bits.Bad() || (len ^ 0xFFFFu) != nlen) return false;
            for (uint32_t i = 0; i < len; ++i) out->push_back(static_cast<uint8_t>(bits.Get(8)));
            if (bits.Bad()) return false;
            continue;
        }

        Huffman litTree, distTree;
        if (type == 1) {
            // 固定 Huffman 表
            uint8_t lengths[kMaxLit] = {0};
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            if (!litTree.Build(lengths, 288)) return false;
            uint8_t dlen[kMaxDist];
            for (int i = 0; i < kMaxDist; ++i) dlen[i] = 5;
            if (!distTree.Build(dlen, kMaxDist)) return false;
        } else if (type == 2) {
            if (!ReadDynamicTrees(&bits, &litTree, &distTree)) return false;
        } else {
            return false;
        }

        for (;;) {
            int sym = litTree.Decode(&bits);
            if (sym < 0 || bits.Bad()) return false;
            if (sym < 256) {
                out->push_back(static_cast<uint8_t>(sym));
                continue;
            }
            if (sym == 256) break;

            const int li = sym - 257;
            if (li >= 29) return false;
            const uint32_t len = kLenBase[li] + bits.Get(kLenExtra[li]);

            int dsym = distTree.Decode(&bits);
            if (dsym < 0 || dsym >= kMaxDist || bits.Bad()) return false;
            const uint32_t dist = kDistBase[dsym] + bits.Get(kDistExtra[dsym]);
            if (dist == 0 || dist > out->size()) return false;

            size_t from = out->size() - dist;
            for (uint32_t i = 0; i < len; ++i) out->push_back((*out)[from + i]);
            if (expectedSize && out->size() > expectedSize + 1024) return false;   // 明显跑飞
        }
    }

    if (expectedSize) return out->size() == expectedSize;
    return !out->empty();
}

}  // namespace adb