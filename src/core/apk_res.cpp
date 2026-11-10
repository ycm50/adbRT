#include "apk_res.h"

#include <cstring>
#include <map>

#include "core.h"

namespace adb {

namespace {

// 资源 chunk 类型
constexpr uint16_t RES_STRING_POOL_TYPE       = 0x0001;
constexpr uint16_t RES_TABLE_TYPE             = 0x0002;
constexpr uint16_t RES_XML_TYPE               = 0x0003;
constexpr uint16_t RES_XML_START_ELEMENT_TYPE = 0x0102;
constexpr uint16_t RES_XML_RESOURCE_MAP_TYPE  = 0x0180;
constexpr uint16_t RES_TABLE_PACKAGE_TYPE     = 0x0200;
constexpr uint16_t RES_TABLE_TYPE_TYPE        = 0x0201;

// Res_value 数据类型
constexpr uint8_t TYPE_REFERENCE = 0x01;
constexpr uint8_t TYPE_STRING    = 0x03;

// android:label 的固定资源 id
constexpr uint32_t ATTR_ANDROID_LABEL = 0x01010001;

// 小工具：按小端读
uint16_t Rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool InRange(size_t off, size_t need, size_t total) { return off + need <= total; }

// ---------------------------------------------------------------- 字符串池
class StringPool {
public:
    bool Parse(const uint8_t* base, size_t size) {
        if (size < 28) return false;
        m_count = Rd32(base + 8);
        m_flags = Rd32(base + 16);
        m_stringsStart = Rd32(base + 20);
        if (!InRange(m_stringsStart, 0, size)) return false;
        m_base = base;
        m_size = size;
        m_ok = true;
        return true;
    }

    std::wstring Get(uint32_t index) const {
        if (!m_ok || index >= m_count) return {};
        const uint8_t* offsets = m_base + 28;   // 头部固定 28 字节
        if (!InRange(28 + index * 4, 4, m_size)) return {};
        uint32_t off = Rd32(offsets + index * 4);
        size_t pos = m_stringsStart + off;
        if (!InRange(pos, 2, m_size)) return {};

        bool utf8 = (m_flags & (1u << 8)) != 0;
        if (utf8) {
            // UTF-16 长度（1 或 2 字节），再是 UTF-8 字节长度（1 或 2 字节）
            size_t p = pos;
            auto skipLen = [&](size_t* adv) -> uint32_t {
                if (!InRange(p, 1, m_size)) return 0;
                uint8_t b = m_base[p];
                if (b & 0x80) {
                    if (!InRange(p, 2, m_size)) return 0;
                    uint32_t v = ((b & 0x7F) << 8) | m_base[p + 1];
                    *adv = 2;
                    return v;
                }
                *adv = 1;
                return b;
            };
            size_t adv = 0;
            skipLen(&adv); p += adv;
            uint32_t byteLen = skipLen(&adv); p += adv;
            if (!InRange(p, byteLen, m_size)) return {};
            return Utf8ToWide(std::string(reinterpret_cast<const char*>(m_base + p), byteLen));
        }

        // UTF-16：长度前缀是 **2 字节**（高位置 1 时扩展为 4 字节），单位是 UTF-16 码元
        size_t p = pos;
        if (!InRange(p, 2, m_size)) return {};
        uint32_t len = Rd16(m_base + p);
        if (len & 0x8000) {
            if (!InRange(p, 4, m_size)) return {};
            len = ((len & 0x7FFF) << 16) | Rd16(m_base + p + 2);
            p += 4;
        } else {
            p += 2;
        }
        if (!InRange(p, len * 2, m_size)) return {};
        std::wstring out(len, L'\0');
        for (uint32_t i = 0; i < len; ++i) {
            out[i] = static_cast<wchar_t>(Rd16(m_base + p + i * 2));
        }
        return out;
    }

    bool Ok() const { return m_ok; }

private:
    const uint8_t* m_base = nullptr;
    size_t   m_size = 0;
    uint32_t m_count = 0;
    uint32_t m_flags = 0;
    uint32_t m_stringsStart = 0;
    bool     m_ok = false;
};

}  // namespace

// ------------------------------------------------------------ AXML（manifest）
bool ParseManifestLabel(const std::vector<uint8_t>& manifest, ManifestLabel* out) {
    if (!out) return false;
    *out = ManifestLabel{};
    if (manifest.size() < 8) return false;
    if (Rd16(manifest.data()) != RES_XML_TYPE) return false;

    const uint8_t* base = manifest.data();
    const size_t total = manifest.size();

    StringPool pool;
    std::vector<uint32_t> resMap;      // 属性名索引 → 资源 id
    bool havePool = false;

    size_t off = Rd16(base + 2);       // 头长度
    while (InRange(off, 8, total)) {
        uint16_t type = Rd16(base + off);
        uint16_t headerSize = Rd16(base + off + 2);
        uint32_t size = Rd32(base + off + 4);
        if (size < 8 || !InRange(off, size, total)) break;

        if (type == RES_STRING_POOL_TYPE && !havePool) {
            havePool = pool.Parse(base + off, size);
        } else if (type == RES_XML_RESOURCE_MAP_TYPE) {
            size_t n = (size - headerSize) / 4;
            resMap.resize(n);
            for (size_t i = 0; i < n; ++i)
                resMap[i] = Rd32(base + off + headerSize + i * 4);
        } else if (type == RES_XML_START_ELEMENT_TYPE) {
            // ResXMLTree_node = chunk头(8) + lineNumber(4) + comment(4)
            // 之后紧接 ResXMLTree_attrExt：ns(4) name(4) attrStart(2) attrSize(2) attrCount(2) ...
            // headerSize 对 START_ELEMENT 来说是 16，指向 attrExt 的起点。
            size_t p = off + headerSize;
            if (!InRange(p, 16, total)) break;
            uint16_t attrStart = Rd16(base + p + 8);
            uint16_t attrSize = Rd16(base + p + 10);
            uint16_t attrCount = Rd16(base + p + 12);

            size_t ab = p + attrStart;
            for (uint16_t i = 0; i < attrCount && attrSize >= 20; ++i) {
                size_t a = ab + static_cast<size_t>(i) * attrSize;
                if (!InRange(a, 20, total)) break;
                uint32_t attrNameIdx = Rd32(base + a + 4);
                uint32_t rawValue = Rd32(base + a + 8);
                uint8_t  dataType = base[a + 15];
                uint32_t data = Rd32(base + a + 16);

                // 判断是不是 android:label：优先用资源映射表，回退到名字
                bool isLabel = false;
                if (attrNameIdx < resMap.size() && resMap[attrNameIdx] == ATTR_ANDROID_LABEL)
                    isLabel = true;
                else if (pool.Get(attrNameIdx) == L"label")
                    isLabel = true;
                if (!isLabel) continue;

                if (dataType == TYPE_STRING) {
                    *out = ManifestLabel{};
                    out->hasLiteral = true;
                    out->literal = pool.Get(rawValue ? rawValue : data);
                    return true;
                }
                if (dataType == TYPE_REFERENCE) {
                    out->resId = data;
                    return true;
                }
            }
        }
        off += size;
    }
    return false;
}

// ------------------------------------------------------- ARSC（resources.arsc）
// ResTable_type 头（headerSize 通常 84，含 config）：
//   +8  id(1)  +9 flags(1)  +10 reserved(2)  +12 entryCount(4)
//   +16 entriesStart(4)  +20 config(...)
//   flags 位 0x01 = FLAG_SPARSE：此时 offset 数组是 (idx(2), offset/4(2)) 对，
//   flags 位 0x02 = FLAG_OFFSET16：offset 数组是 2 字节的 offset/4
//   本函数三种编码都要能读，否则国产 ROM 的 arsc 会整片查不到。
namespace {

struct ResEntry {
    uint16_t size = 0;
    uint16_t flags = 0;
    uint8_t  dataType = 0;
    uint32_t data = 0;
    bool     simple = false;
};

// 在单个 type chunk 里按 index 取条目
bool ReadEntry(const uint8_t* base, size_t total, size_t typeOff, uint16_t headerSize,
               uint32_t typeSize, uint8_t flags, uint32_t entryCount, uint32_t entriesStart,
               uint16_t wantIndex, ResEntry* out) {
    size_t offsBase = typeOff + headerSize;
    size_t entryOff = 0;
    bool found = false;

    if (flags & 0x01) {
        // FLAG_SPARSE：每项 4 字节 = idx(2) + offset/4(2)
        size_t n = (typeSize > headerSize) ? (typeSize - headerSize) / 4 : 0;
        for (size_t i = 0; i < n; ++i) {
            size_t q = offsBase + i * 4;
            if (q + 4 > total) return false;
            if (Rd16(base + q) != wantIndex) continue;
            entryOff = static_cast<size_t>(Rd16(base + q + 2)) * 4;
            found = true;
            break;
        }
    } else if (flags & 0x02) {
        // FLAG_OFFSET16：2 字节，值 = offset/4，0xFFFF 表示"跳过"
        if (wantIndex >= entryCount) return false;
        size_t q = offsBase + static_cast<size_t>(wantIndex) * 2;
        if (q + 2 > total) return false;
        uint16_t v = Rd16(base + q);
        if (v == 0xFFFF) return false;
        entryOff = static_cast<size_t>(v) * 4;
        found = true;
    } else {
        if (wantIndex >= entryCount) return false;
        size_t q = offsBase + static_cast<size_t>(wantIndex) * 4;
        if (q + 4 > total) return false;
        uint32_t v = Rd32(base + q);
        if (v == 0xFFFFFFFFu) return false;
        entryOff = v;
        found = true;
    }
    if (!found) return false;

    size_t e = typeOff + entriesStart + entryOff;
    if (e + 8 > total) return false;
    out->size = Rd16(base + e);
    out->flags = Rd16(base + e + 2);
    out->simple = (out->flags & 0x0001) == 0;
    if (out->simple) {
        size_t v = e + out->size;
        if (v + 8 > total) return false;
        out->dataType = base[v + 3];
        out->data = Rd32(base + v + 4);
    } else {
        // 复杂条目（如 plural / array）：取它的第一项
        size_t c = e + out->size;
        if (c + 12 > total) return false;
        uint32_t count = Rd32(base + c + 8);
        if (count == 0) return false;
        size_t v = c + 12;
        if (v + 8 > total) return false;
        out->size = Rd16(base + v);
        out->simple = true;
        out->dataType = base[v + 3];
        out->data = Rd32(base + v + 4);
    }
    return true;
}

}  // namespace

bool LookupStringResource(const std::vector<uint8_t>& arsc, uint32_t resId, std::wstring* out,
                          int depth) {
    if (!out || arsc.size() < 12 || depth > 4) return false;
    out->clear();
    if (Rd16(arsc.data()) != RES_TABLE_TYPE) return false;

    const uint8_t* base = arsc.data();
    const size_t total = arsc.size();

    StringPool globalPool;
    if (!globalPool.Parse(base + Rd16(base + 2), total - Rd16(base + 2))) return false;
    if (!globalPool.Ok()) return false;

    const uint8_t wantType = static_cast<uint8_t>((resId >> 16) & 0xFF);
    const uint16_t wantIndex = static_cast<uint16_t>(resId & 0xFFFF);

    size_t off = Rd16(base + 2);
    while (InRange(off, 8, total)) {
        uint16_t type = Rd16(base + off);
        uint16_t headerSize = Rd16(base + off + 2);
        uint32_t size = Rd32(base + off + 4);
        if (size < 8 || !InRange(off, size, total)) break;

        if (type == RES_TABLE_PACKAGE_TYPE) {
            size_t pOff = off + headerSize;
            size_t pEnd = off + size;
            while (pOff + 8 <= pEnd) {
                uint16_t t2 = Rd16(base + pOff);
                uint16_t h2 = Rd16(base + pOff + 2);
                uint32_t s2 = Rd32(base + pOff + 4);
                if (s2 < 8 || pOff + s2 > pEnd) break;

                if (t2 == RES_TABLE_TYPE_TYPE) {
                    uint8_t typeId = base[pOff + 8];
                    uint8_t flags = base[pOff + 9];
                    uint32_t entryCount = Rd32(base + pOff + 12);
                    uint32_t entriesStart = Rd32(base + pOff + 16);

                    if (typeId == wantType) {
                        ResEntry e;
                        if (ReadEntry(base, total, pOff, h2, s2, flags, entryCount,
                                      entriesStart, wantIndex, &e) && e.simple) {
                            if (e.dataType == TYPE_STRING) {
                                *out = globalPool.Get(e.data);
                                return !out->empty();
                            }
                            if (e.dataType == TYPE_REFERENCE && e.data != resId) {
                                return LookupStringResource(arsc, e.data, out, depth + 1);
                            }
                        }
                    }
                }
                pOff += s2;
            }
        }
        off += size;
    }
    return false;
}

std::wstring ResolveAppLabel(const std::vector<uint8_t>& manifest,
                             const std::vector<uint8_t>& arsc) {
    ManifestLabel ml;
    if (!ParseManifestLabel(manifest, &ml)) return {};
    if (ml.hasLiteral && !ml.literal.empty()) return ml.literal;
    if (ml.resId == 0) return {};
    if (arsc.empty()) return {};
    std::wstring s;
    if (LookupStringResource(arsc, ml.resId, &s)) return s;
    return {};
}



}  // namespace adb