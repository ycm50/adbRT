#pragma once
// 测试用的最小 AXML + ARSC 样本（由 tmpscripts/mkfixture.py 同构生成）
//   · manifest 里 <application android:label="@string/...">
//   · arsc 里 0x7f010000 指向 "AdbRT 测试应用"
// 放在源码里而不是二进制文件：方便阅读与修改，也避免仓库里出现不透明 blob。

#include <cstdint>
#include <vector>

namespace adbtest {

inline void App(std::vector<uint8_t>& v, std::initializer_list<uint8_t> bytes) {
    v.insert(v.end(), bytes);
}

inline void U16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

inline void U32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

inline void Pad4(std::vector<uint8_t>& v) { while (v.size() % 4) v.push_back(0); }

inline void U16String(std::vector<uint8_t>& v, const wchar_t* s) {
    size_t n = 0;
    while (s[n]) ++n;
    U16(v, static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; ++i) U16(v, static_cast<uint16_t>(s[i]));
    U16(v, 0);
}

// 一个只含 ASCII 名的字符串池
inline std::vector<uint8_t> StringPool(const wchar_t* const* strings, size_t count) {
    std::vector<uint8_t> data;
    std::vector<uint32_t> offsets;
    for (size_t i = 0; i < count; ++i) {
        offsets.push_back(static_cast<uint32_t>(data.size()));
        U16String(data, strings[i]);
    }
    Pad4(data);

    std::vector<uint8_t> out;
    U16(out, 0x0001);
    U16(out, 28);
    U32(out, static_cast<uint32_t>(28 + count * 4 + data.size()));
    U32(out, static_cast<uint32_t>(count));
    U32(out, 0);
    U32(out, 0x100);   // UTF-8 标志？这里用 UTF-16，置 0
    out[16] = 0; out[17] = 0; out[18] = 0; out[19] = 0;
    U32(out, static_cast<uint32_t>(28 + count * 4));
    U32(out, 0);
    for (uint32_t o : offsets) U32(out, o);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

inline std::vector<uint8_t> FixtureManifest(uint32_t resId) {
    const wchar_t* names[] = {L"android", L"manifest", L"application", L"label", L"", L"name"};
    std::vector<uint8_t> out;
    U16(out, 0x0003);   // RES_XML_TYPE
    U16(out, 8);
    U32(out, 8);
    auto pool = StringPool(names, 6);
    out.insert(out.end(), pool.begin(), pool.end());

    // RES_XML_RESOURCE_MAP_TYPE：索引 3（label）→ android:label 资源 id
    U16(out, 0x0180);
    U16(out, 8);
    U32(out, 8 + 4 * 4);
    U32(out, 0); U32(out, 0); U32(out, 0); U32(out, 0x01010001);

    auto startElem = [&](uint32_t nameIdx, bool withLabel) {
        std::vector<uint8_t> body;
        U32(body, 0xFFFFFFFF); U32(body, 0xFFFFFFFF); U32(body, 0xFFFFFFFF);
        U32(body, nameIdx);
        U16(body, 20); U16(body, 20);
        U16(body, withLabel ? 1 : 0);
        U16(body, 0); U16(body, 0); U16(body, 0); U16(body, 0);
        if (withLabel) {
            U32(body, 0);          // ns
            U32(body, 3);          // name = "label"
            U32(body, 0xFFFFFFFF); // raw
            U16(body, 8);          // size
            body.push_back(0);     // res0
            body.push_back(0x01);  // TYPE_REFERENCE
            U32(body, resId);
        }
        std::vector<uint8_t> chunk;
        U16(chunk, 0x0102);
        U16(chunk, 16);
        U32(chunk, static_cast<uint32_t>(8 + body.size()));
        U32(chunk, 1);
        chunk.insert(chunk.end(), body.begin(), body.end());
        return chunk;
    };

    auto m = startElem(1, false);   // <manifest>
    out.insert(out.end(), m.begin(), m.end());
    auto a = startElem(2, true);    // <application android:label=@resId>
    out.insert(out.end(), a.begin(), a.end());
    return out;
}

inline std::vector<uint8_t> FixtureArsc(uint32_t resId, const wchar_t* text) {
    const wchar_t* strings[] = {text};
    auto pool = StringPool(strings, 1);

    uint8_t typeId = static_cast<uint8_t>((resId >> 16) & 0xFF);
    uint16_t index = static_cast<uint16_t>(resId & 0xFFFF);

    // 一条 entry：ResTable_entry(8) + Res_value(8)
    std::vector<uint8_t> entry;
    U16(entry, 8); U16(entry, 0); U32(entry, 0);
    U16(entry, 8); entry.push_back(0); entry.push_back(0x03); U32(entry, 0);

    // type 块：头 0x40 + 偏移表
    std::vector<uint8_t> tbody;
    tbody.push_back(typeId); tbody.push_back(0); U16(tbody, 0);
    U32(tbody, static_cast<uint32_t>(index + 1));
    U32(tbody, 0);       // entriesStart
    U32(tbody, 0);
    for (int i = 0; i <= index; ++i) U32(tbody, i == 0 ? 0 : 0xFFFFFFFFu);
    while (tbody.size() < 0x40) tbody.push_back(0);

    std::vector<uint8_t> typeChunk;
    U16(typeChunk, 0x0201);
    U16(typeChunk, 0x40);
    U32(typeChunk, static_cast<uint32_t>(8 + tbody.size() + entry.size()));
    typeChunk.push_back(1);
    tbody.insert(tbody.begin() + 8, typeChunk.begin() + 8, typeChunk.end());  // 占位，下面重算
    typeChunk.clear();

    // 重新组装（保持简单可读）
    std::vector<uint8_t> tb;
    tb.push_back(typeId); tb.push_back(0); U16(tb, 0);
    U32(tb, static_cast<uint32_t>(index + 1));
    U32(tb, 0);
    U32(tb, 0);
    for (int i = 0; i <= index; ++i) U32(tb, i == 0 ? 0 : 0xFFFFFFFFu);
    while (tb.size() < 0x40) tb.push_back(0);
    tb.insert(tb.end(), entry.begin(), entry.end());

    std::vector<uint8_t> tChunk;
    U16(tChunk, 0x0201);
    U16(tChunk, 0x40);
    U32(tChunk, static_cast<uint32_t>(8 + tb.size()));
    U32(tChunk, 1);
    tChunk.insert(tChunk.end(), tb.begin(), tb.end());

    // package 块：头 0x120
    std::vector<uint8_t> pkg;
    U16(pkg, 0x0200);
    U16(pkg, 0x120);
    U32(pkg, 0);
    U32(pkg, 0x7F);
    // typeName[128] UTF-16
    const wchar_t* tname = L"string";
    size_t ti = 0;
    for (; tname[ti] && ti < 127; ++ti) U16(pkg, static_cast<uint16_t>(tname[ti]));
    for (size_t k = ti; k < 128; ++k) U16(pkg, 0);
    U32(pkg, 0);   // typeIdOffset
    pkg.insert(pkg.end(), tChunk.begin(), tChunk.end());
    // 回填大小
    uint32_t pkgSize = static_cast<uint32_t>(pkg.size());
    pkg[4] = static_cast<uint8_t>(pkgSize & 0xFF);
    pkg[5] = static_cast<uint8_t>((pkgSize >> 8) & 0xFF);
    pkg[6] = static_cast<uint8_t>((pkgSize >> 16) & 0xFF);
    pkg[7] = static_cast<uint8_t>((pkgSize >> 24) & 0xFF);

    std::vector<uint8_t> out;
    U16(out, 0x0002);
    U16(out, 12);
    U32(out, static_cast<uint32_t>(12 + pool.size() + pkg.size()));
    U32(out, 1);
    out.insert(out.end(), pool.begin(), pool.end());
    out.insert(out.end(), pkg.begin(), pkg.end());
    return out;
}

}  // namespace adbtest
