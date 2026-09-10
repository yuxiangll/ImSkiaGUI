// ============================================================================
//  gallery/Echo.cpp — 实现
// ============================================================================
#include "gallery/Echo.h"

#include <cstring>

namespace gallery {

void Echo::Add(std::function<void(char*, std::size_t)> fn, skiagui::uikit::Text* target) {
    if (!fn || !target) return;
    Entry e;
    e.fn = std::move(fn);
    e.target = target;
    entries_.push_back(std::move(e));
}

void Echo::Clear() {
    entries_.clear();
    nextUpdate_ = 0.0;
}

void Echo::Update(double nowSeconds) {
    if (entries_.empty()) return;
    if (nowSeconds < nextUpdate_) return;
    nextUpdate_ = nowSeconds + kIntervalSeconds;

    for (Entry& e : entries_) {
        char tmp[kBufSize] = {};
        e.fn(tmp, kBufSize);
        tmp[kBufSize - 1] = '\0';
        if (std::strcmp(tmp, e.buf) != 0) {
            std::memcpy(e.buf, tmp, kBufSize);
            if (e.target) e.target->setText(e.buf);
        }
    }
}

}  // namespace gallery
