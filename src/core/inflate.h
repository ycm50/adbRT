#pragma once
// ---------------------------------------------------------------------------
// 极简 raw-deflate 解压（RFC 1951）
//   · APK 里的 AndroidManifest.xml 通常是 deflate 压缩的，不解压就读不到 label
//   · 只有解压、没有压缩；不依赖 zlib，与项目其它部分一样零第三方依赖
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

namespace adb {

// 输入是 raw deflate 流（不含 zlib 头），输出解压后的字节
bool InflateRaw(const uint8_t* in, size_t inSize, size_t expectedSize, std::vector<uint8_t>* out);

}  // namespace adb
