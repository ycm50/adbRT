#pragma once
// ---------------------------------------------------------------------------
// BoundedText：按字节预算的"丢旧留新"文本缓冲
//
// 解决的问题：GUI 里所有"把输出往回贴"的地方（运行结果框、实时日志）
// 原来都是 std::wstring / std::vector<std::wstring> 一路 push_back，
// 只有下限没有上限。跑一次 logcat 就几百 MB，主线程还要在这堆越来越长的
// 文本上做 EM_REPLACESEL + 全选滚动 —— 这就是"越用越卡"的来源。
//
// 设计约束（写在类型里，别在调用点重复实现）：
//   1. 生产端永不阻塞：Append 只做定长搬运，绝不等待消费者；
//   2. 超预算丢最旧的：整块裁掉，摊还 O(1)，不做逐字节搬迁；
//   3. 消费端批量取走：TakeBatch 一次给一批，让 UI 一次只做一次追加；
//   4. 单行有上限：超长行先截断，避免一行就把预算吃光。
//
// 纯标准库实现，不依赖 Win32，所以可以直接进无窗口单元测试。
// ---------------------------------------------------------------------------

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace adb {

class BoundedText {
public:
    explicit BoundedText(size_t maxBytes = 8 * 1024 * 1024,
                         size_t maxLineBytes = 8 * 1024)
        : m_maxBytes(maxBytes ? maxBytes : 1),
          m_maxLine(maxLineBytes ? maxLineBytes : 1) {}

    // 追加一行（不含换行符）；超出预算时丢最旧的内容。
    // 返回 false 表示这一行因为超长被截断了。
    bool Append(const std::wstring& line);

    // 取走并清空累积的内容，拼成一块带 CRLF 的文本。
    // limitBytes > 0 时最多取这么多字节（UTF-16 计），剩下的留在缓冲里。
    std::wstring TakeBatch(size_t limitBytes = 0);

    // 清空内容（重新开始计数）
    void Clear();

    // 累计追加过的行数（包含已经被丢弃的）。配合 PendingLines() 就能算出
    // "当前缓冲区里最早的那一行当初是第几行"，增量渲染据此判断要不要重建。
    size_t TotalLines() const;

    // 把当前缓冲里 [fromLine, fromLine+maxLines) 的行复制出去（fromLine 相对缓冲起点）。
    // maxLines = 0 表示到末尾。
    void CopyLines(size_t fromLine, size_t maxLines, std::vector<std::wstring>* out) const;

    size_t PendingBytes() const;
    size_t PendingLines() const;
    // 因超出预算而丢掉的内容，用来在界面上告诉用户"已丢弃多少"
    size_t DroppedBytes() const;
    // 有内容被截断过（超长行）
    bool   Truncated() const;

private:
    void TrimLocked();

    mutable std::mutex      m_mu;
    std::deque<std::wstring> m_lines;
    size_t                  m_bytes = 0;      // 内容占用的 UTF-16 单位数
    size_t                  m_maxBytes;
    size_t                  m_maxLine;
    size_t                  m_dropped = 0;
    size_t                  m_total = 0;      // 累计追加行数（含已丢弃）
    bool                    m_truncated = false;
};

}  // namespace adb
