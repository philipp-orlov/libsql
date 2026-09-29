// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- errors raised by the SQL layer.
#pragma once

#include <stdexcept>
#include <string>

namespace sql {

enum class ErrorCode
{
    Ok = 0,
    SyntaxError,      ///< the statement did not parse
    Unsupported,      ///< valid SQL that this engine does not implement
    InvalidArgument,  ///< bad parameter count, malformed literal, ...
    NoSuchTable,
    TableExists,
    NoSuchColumn,
    AmbiguousColumn,  ///< a bare name that two joined tables both provide
    NoSuchIndex,
    IndexExists,
    TypeMismatch,          ///< a value cannot be stored in the declared type
    ConstraintViolation,   ///< NOT NULL, PRIMARY KEY or UNIQUE rejected a row
    BadTransaction,        ///< use of a committed or rolled-back transaction
    ReadOnly,
    Internal,
};

const char* toString(ErrorCode) noexcept;

/// Every failure path in libsql throws this. Storage-level failures keep
/// arriving as nosql::Error.
class Error : public std::runtime_error
{
public:
    Error(ErrorCode code, const std::string& what) : std::runtime_error(what), code_(code) {}
    explicit Error(ErrorCode code) : std::runtime_error(toString(code)), code_(code) {}

    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

}  // namespace sql
