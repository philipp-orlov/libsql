// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace conformance {

struct GeneratedStatement
{
    std::string sql;
    bool checkState = false;
    std::string table;
};

class SqlGenerator
{
public:
    explicit SqlGenerator(std::uint64_t seed);

    const std::vector<std::string>& setup() const noexcept { return setup_; }
    GeneratedStatement next();
    std::string stateCheck(const std::string& table) const;

private:
    int pick(int upper);
    bool chance(int numerator, int denominator);
    std::string literal();
    std::string column(const std::string& table);
    std::string expression(const std::string& table, int depth);
    std::string predicate(const std::string& table);
    std::string tableName();

    std::mt19937_64 rng_;
    std::vector<std::string> setup_;
};

}  // namespace conformance
