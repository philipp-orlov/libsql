// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What RAISE() throws inside a trigger.
#pragma once

#include <string>

#include "internal/ast.hpp"
#include "sql/error.hpp"

namespace sql::internal {

/// RAISE(IGNORE): abandon the current row of the statement that fired the
/// trigger, silently.
struct TriggerIgnore
{
};

/// RAISE(ROLLBACK|ABORT|FAIL, message).
class RaiseError : public Error
{
public:
    RaiseError(RaiseKind kind, const std::string& message)
        : Error(ErrorCode::ConstraintViolation, Constraint::Trigger, message), kind_(kind)
    {}
    RaiseKind kind() const noexcept { return kind_; }

private:
    RaiseKind kind_;
};

/// A conflict resolved with FAIL: the statement stops but keeps the rows it
/// changed before the failing one.
class FailError : public Error
{
public:
    explicit FailError(const Error& e) : Error(e) {}
};

/// A conflict resolved with ROLLBACK: the whole transaction is abandoned.
class RollbackError : public Error
{
public:
    explicit RollbackError(const Error& e) : Error(e) {}
};

}  // namespace sql::internal
