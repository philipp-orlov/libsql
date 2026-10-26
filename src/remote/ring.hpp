// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A circular queue of fixed capacity: allocated once, never resized. A std::deque allocates blocks as it
// grows and frees them as it shrinks, which is the churn a long-running server must not have.
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace sql::remote::detail {

template <class T>
class Ring
{
public:
    Ring() = default;
    explicit Ring(std::size_t capacity) : slots_(capacity ? capacity : 1) {}

    std::size_t capacity() const noexcept { return slots_.size(); }
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    bool full() const noexcept { return count_ == slots_.size(); }

    /// False when full (the element is left alone).
    bool push(T&& value)
    {
        if (full())
            return false;
        slots_[(head_ + count_) % slots_.size()] = std::move(value);
        ++count_;
        return true;
    }
    T& front() { return slots_[head_]; }
    T pop()
    {
        T out = std::move(slots_[head_]);
        slots_[head_] = T();
        head_ = (head_ + 1) % slots_.size();
        --count_;
        return out;
    }
    /// The i-th element from the front.
    T& at(std::size_t i) { return slots_[(head_ + i) % slots_.size()]; }
    void clear()
    {
        while (count_)
            pop();
        head_ = 0;
    }

private:
    std::vector<T> slots_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

}  // namespace sql::remote::detail
