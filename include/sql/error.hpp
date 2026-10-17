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
    Runtime,      ///< an error raised while running: integer overflow, malformed JSON, RAISE
    Busy,         ///< a deferred transaction could not become a writer, or a store is in use
    Interrupted,  ///< Database::interrupt() stopped the statement
};

/// Which constraint a ConstraintViolation came from, as SQLite's extended
/// result codes tell them apart.
enum class Constraint
{
    None = 0,
    PrimaryKey,
    Unique,
    NotNull,
    Check,
    ForeignKey,
    Trigger,   ///< RAISE(ABORT|FAIL|ROLLBACK, ...)
    Datatype,  ///< a STRICT column refused a value
};

const char* toString(ErrorCode) noexcept;

/// Every failure path in libsql throws this. Storage-level failures keep
/// arriving as nosql::Error.
class Error : public std::runtime_error
{
public:
    Error(ErrorCode code, const std::string& what) : std::runtime_error(what), code_(code) {}
    Error(ErrorCode code, Constraint constraint, const std::string& what)
        : std::runtime_error(what), code_(code), constraint_(constraint)
    {}
    explicit Error(ErrorCode code) : std::runtime_error(toString(code)), code_(code) {}

    ErrorCode code() const noexcept { return code_; }
    /// For ConstraintViolation, the kind of constraint; None otherwise.
    Constraint constraint() const noexcept { return constraint_; }

private:
    ErrorCode code_;
    Constraint constraint_ = Constraint::None;
};

}  // namespace sql
