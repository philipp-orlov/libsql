// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The report definitions the sql tool serves: one JSON file per report in a directory next to the server. The front end
// designs and runs them; the server only stores them, so it checks what keeps the directory safe (the name, that the text
// is JSON, its size) and leaves the meaning of the document to the front end.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sqlweb {

struct ReportInfo
{
    std::string name;
    std::uint64_t size = 0;
    /// Seconds since the epoch.
    std::int64_t modified = 0;
};

class ReportStore
{
public:
    /// The largest definition accepted, in bytes.
    static constexpr std::size_t kMaxBytes = 1u << 20;

    explicit ReportStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

    /// 1 to 80 of letters, digits, space, '_', '-', '.', not starting with a space or a dot, not ending in a dot, and
    /// without "..": a safe file name on every platform.
    static bool validName(std::string_view name);

    /// True when `text` is one well-formed JSON value (strict RFC 8259 syntax, nesting up to 64 deep).
    static bool validJson(std::string_view text);

    /// The reports, by name. A missing directory is an empty list.
    std::vector<ReportInfo> list() const;
    std::optional<std::string> read(std::string_view name) const;
    /// Writes through a temporary file and a rename, so a reader or a crash never sees half a report. Throws
    /// std::runtime_error when it cannot.
    void write(std::string_view name, std::string_view json) const;
    /// False when there was no such report.
    bool remove(std::string_view name) const;

    const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    std::filesystem::path file(std::string_view name) const;
    std::filesystem::path directory_;
};

}  // namespace sqlweb
