// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A bump allocator for objects that live and die together: the expression nodes of one parsed statement. Allocation is
// a pointer bump inside the current chunk; nothing is freed until the arena goes. The chunks now come from libcore's
// mem::Arena over the process-wide size-class pool, so a dropped statement hands its chunks back to a bounded, shared
// pool instead of leaving holes in the heap, and a statement's tree sits in a few contiguous, cache-line aligned blocks.
#pragma once

#include <cstddef>

#include "mem/arena.hpp"

namespace sql::internal {

class Arena
{
public:
    explicit Arena(std::size_t blockSize) : arena_(options(blockSize)) {}
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(std::size_t size, std::size_t align) { return arena_.allocate(size, align); }

    /// Bytes held in chunks.
    std::size_t capacity() const noexcept { return arena_.chunkCount() * arena_.chunkBytes(); }

private:
    static mem::Arena::Options options(std::size_t blockSize)
    {
        mem::Arena::Options o;
        o.chunkBytes = blockSize < 4096 ? 4096 : blockSize;
        o.retainChunks = 0;
        return o;
    }

    mem::Arena arena_;
};

}  // namespace sql::internal
