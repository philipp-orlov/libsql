// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A bump allocator for objects that live and die together: the expression
// nodes of one parsed statement. Allocation is a pointer bump inside the
// current block; nothing is freed until the arena goes, when every block is
// released at once. A statement's tree then sits in a few contiguous blocks
// instead of dozens of separate heap allocations, which is cheaper to build,
// friendlier to the cache while evaluating, and leaves no small holes behind
// in the heap when the statement is dropped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

namespace sql::internal {

class Arena
{
public:
    explicit Arena(std::size_t blockSize) noexcept : blockSize_(blockSize < 256 ? 256 : blockSize) {}
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena()
    {
        while (head_) {
            Block* next = head_->next;
            ::operator delete(head_);
            head_ = next;
        }
    }

    void* allocate(std::size_t size, std::size_t align)
    {
        std::uintptr_t p = (std::uintptr_t(cur_) + (align - 1)) & ~std::uintptr_t(align - 1);
        if (!cur_ || p + size > std::uintptr_t(end_)) {
            grow(size + align);
            p = (std::uintptr_t(cur_) + (align - 1)) & ~std::uintptr_t(align - 1);
        }
        cur_ = reinterpret_cast<char*>(p + size);
        return reinterpret_cast<void*>(p);
    }

    /// Bytes held in blocks.
    std::size_t capacity() const noexcept { return held_; }

private:
    struct Block
    {
        Block* next;
    };

    void grow(std::size_t need)
    {
        // A node bigger than a block gets a block of its own.
        const std::size_t bytes = sizeof(Block) + (need > blockSize_ ? need : blockSize_);
        Block* b = static_cast<Block*>(::operator new(bytes));
        b->next = head_;
        head_ = b;
        cur_ = reinterpret_cast<char*>(b + 1);
        end_ = reinterpret_cast<char*>(b) + bytes;
        held_ += bytes;
    }

    std::size_t blockSize_;
    Block* head_ = nullptr;
    char* cur_ = nullptr;
    char* end_ = nullptr;
    std::size_t held_ = 0;
};

}  // namespace sql::internal
