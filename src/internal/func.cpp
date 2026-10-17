// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/func.hpp"

#include <chrono>

#include "internal/util.hpp"

namespace sql::internal {

std::int64_t FuncContext::now()
{
    if (nowUsec == INT64_MIN) {
        nowUsec = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    }
    return nowUsec;
}

void FuncContext::error(const std::string& message) const
{
    throw Error(ErrorCode::Runtime, message);
}

void FuncContext::tooBig() const
{
    throw Error(ErrorCode::Runtime, "string or blob too big");
}

void AggregateState::inverse(FuncContext&, int, Args)
{
    throw Error(ErrorCode::Internal, "aggregate cannot be inverted");
}

void FunctionRegistry::add(FunctionDef def)
{
    def.name = toLower(def.name);
    byName_[def.name].push_back(functions_.size());
    functions_.push_back(std::move(def));
}

void FunctionRegistry::addTable(TableFunctionDef def)
{
    def.name = toLower(def.name);
    tables_.push_back(std::move(def));
}

const FunctionDef* FunctionRegistry::find(std::string_view name, int argc) const
{
    const auto it = byName_.find(toLower(name));
    if (it == byName_.end())
        return nullptr;
    const FunctionDef* variadic = nullptr;
    for (const std::size_t i : it->second) {
        const FunctionDef& f = functions_[i];
        if (f.minArgs == f.maxArgs && f.minArgs == argc)
            return &f;
        if (argc >= f.minArgs && (f.maxArgs < 0 || argc <= f.maxArgs) && !variadic)
            variadic = &f;
    }
    return variadic;
}

bool FunctionRegistry::hasName(std::string_view name) const
{
    return byName_.count(toLower(name)) != 0;
}

const TableFunctionDef* FunctionRegistry::findTable(std::string_view name) const
{
    for (const TableFunctionDef& t : tables_) {
        if (equalsNoCase(t.name, name))
            return &t;
    }
    return nullptr;
}

const FunctionRegistry& FunctionRegistry::builtins()
{
    static const FunctionRegistry registry = [] {
        FunctionRegistry r;
        registerCoreFunctions(r);
        registerMathFunctions(r);
        registerAggregateFunctions(r);
        registerJsonFunctions(r);
        registerDateFunctions(r);
        registerPrintfFunctions(r);
        return r;
    }();
    return registry;
}

}  // namespace sql::internal
