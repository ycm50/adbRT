#include "bounded_text.h"

namespace adb {

bool BoundedText::Append(const std::wstring& line) {
    bool truncated = false;
    std::wstring item = line;
    if (item.size() > m_maxLine) {
        item.resize(m_maxLine);
        item += L"...[已截断]";
        truncated = true;
    }

    std::lock_guard<std::mutex> lk(m_mu);
    if (truncated) m_truncated = true;

    // 单行本身就超预算：只留它的尾巴（最新的信息最有用）
    if (item.size() > m_maxBytes) {
        const size_t cut = item.size() - m_maxBytes;
        m_dropped += cut;
        item.erase(0, cut);
    }

    m_bytes += item.size();
    m_lines.push_back(std::move(item));
    ++m_total;
    TrimLocked();
    return !truncated;
}

void BoundedText::TrimLocked() {
    // 一次裁掉"超出预算的那部分"，为了摊还成本再额外裁掉一半容量：
    // 这样每 N 次 Append 才发生一次真正的搬运，而不是每次都动 deque。
    if (m_bytes <= m_maxBytes) return;
    const size_t target = m_maxBytes / 2;
    while (!m_lines.empty() && m_bytes > target) {
        const size_t n = m_lines.front().size();
        m_bytes -= n;
        m_dropped += n;
        m_lines.pop_front();
    }
}

std::wstring BoundedText::TakeBatch(size_t limitBytes) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_lines.empty()) return {};

    std::wstring out;
    if (limitBytes == 0 || limitBytes >= m_bytes) {
        out.reserve(m_bytes + m_lines.size() * 2);
        for (const auto& l : m_lines) {
            out += l;
            out += L"\r\n";
        }
        m_lines.clear();
        m_bytes = 0;
        return out;
    }

    // 只取前面若干行，剩下的留着下次取
    out.reserve(limitBytes + 64);
    while (!m_lines.empty() && out.size() < limitBytes) {
        const std::wstring& l = m_lines.front();
        out += l;
        out += L"\r\n";
        m_bytes -= l.size();
        m_lines.pop_front();
    }
    return out;
}

void BoundedText::Clear() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lines.clear();
    m_bytes = 0;
    m_dropped = 0;
    m_total = 0;
    m_truncated = false;
}

size_t BoundedText::TotalLines() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_total;
}

void BoundedText::CopyLines(size_t fromLine, size_t maxLines, std::vector<std::wstring>* out) const {
    if (!out) return;
    out->clear();
    std::lock_guard<std::mutex> lk(m_mu);
    if (fromLine >= m_lines.size()) return;
    size_t end = (maxLines == 0) ? m_lines.size() : fromLine + maxLines;
    if (end > m_lines.size()) end = m_lines.size();
    out->reserve(end - fromLine);
    for (size_t i = fromLine; i < end; ++i) out->push_back(m_lines[i]);
}

size_t BoundedText::PendingBytes() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_bytes;
}

size_t BoundedText::PendingLines() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_lines.size();
}

size_t BoundedText::DroppedBytes() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_dropped;
}

bool BoundedText::Truncated() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_truncated;
}

}  // namespace adb
