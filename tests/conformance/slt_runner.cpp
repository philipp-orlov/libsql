// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "conformance_util.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

using conformance::ExecResult;
using conformance::LibsqlDb;
using conformance::QueryResult;
using conformance::SqliteDb;

namespace {

struct Options
{
    std::string engine = "libsql";
    bool verbose = false;
    int maxFailures = -1;
    std::filesystem::path skipList;
    std::filesystem::path summaryJson;
    std::vector<std::string> inputs;
};

struct SkipEntry
{
    std::string pattern;
    int line = 0;
    std::string reason;
};

struct Record
{
    enum class Kind
    {
        Statement,
        Query,
    };

    Kind kind = Kind::Statement;
    int line = 0;
    std::string expectation;
    std::string types;
    std::string sortMode = "nosort";
    std::string label;
    std::string sql;
    std::vector<std::string> expected;
    std::vector<std::string> skipIf;
    std::vector<std::string> onlyIf;
    int hashThreshold = 8;
};

struct FileStats
{
    std::string path;
    int run = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
};

struct ParseState
{
    int hashThreshold = 8;
    bool halted = false;
    std::vector<std::string> pendingSkipIf;
    std::vector<std::string> pendingOnlyIf;
};

class Engine
{
public:
    virtual ~Engine() = default;
    virtual ExecResult exec(const std::string& sql) = 0;
};

class SqliteEngine final : public Engine
{
public:
    SqliteEngine() = default;
    ExecResult exec(const std::string& sql) override { return db_.exec(sql); }

private:
    SqliteDb db_;
};

class LibsqlEngine final : public Engine
{
public:
    explicit LibsqlEngine(const std::filesystem::path& dbPath) : db_(dbPath) {}
    ExecResult exec(const std::string& sql) override { return db_.exec(sql); }

private:
    LibsqlDb db_;
};

std::vector<std::string> splitWords(const std::string& line)
{
    std::istringstream in(line);
    std::vector<std::string> words;
    std::string word;
    while (in >> word)
        words.push_back(word);
    return words;
}

bool isDigits(const std::string& text)
{
    if (text.empty())
        return false;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
    }
    return true;
}

std::vector<SkipEntry> loadSkipList(const std::filesystem::path& path)
{
    std::vector<SkipEntry> entries;
    if (path.empty())
        return entries;
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open skip list " + path.string());

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::size_t first = 0;
        while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first])))
            ++first;
        if (first == line.size() || line[first] == '#')
            continue;
        std::size_t split = first;
        while (split < line.size() && !std::isspace(static_cast<unsigned char>(line[split])))
            ++split;
        SkipEntry entry;
        std::string pattern = line.substr(first, split - first);
        const std::size_t colon = pattern.rfind(':');
        if (colon != std::string::npos && isDigits(pattern.substr(colon + 1))) {
            entry.line = std::atoi(pattern.substr(colon + 1).c_str());
            pattern.erase(colon);
        }
        entry.pattern = pattern;
        std::size_t reason = split;
        while (reason < line.size() && std::isspace(static_cast<unsigned char>(line[reason])))
            ++reason;
        entry.reason = line.substr(reason);
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool skipListMatches(const std::vector<SkipEntry>& entries, const std::filesystem::path& file,
                     int line, std::string* reason)
{
    const std::string full = file.lexically_normal().string();
    const std::string name = file.filename().string();
    for (const SkipEntry& entry : entries) {
        if (entry.line != 0 && entry.line != line)
            continue;
        if (entry.pattern == full || entry.pattern == name ||
            conformance::wildcardMatch(entry.pattern, full) ||
            conformance::wildcardMatch(entry.pattern, name)) {
            if (reason)
                *reason = entry.reason;
            return true;
        }
    }
    return false;
}

bool conditionApplies(const std::vector<std::string>& conditions)
{
    for (const std::string& condition : conditions) {
        if (condition == "sqlite")
            return true;
    }
    return false;
}

bool onlyIfAllows(const std::vector<std::string>& conditions)
{
    return conditions.empty() || conditionApplies(conditions);
}

std::vector<Record> parseFile(const std::filesystem::path& path, ParseState* state)
{
    std::vector<Record> records;
    std::istringstream in(conformance::readFile(path));
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }

    for (std::size_t i = 0; i < lines.size();) {
        const std::string& current = lines[i];
        const std::vector<std::string> words = splitWords(current);
        if (words.empty() || (!current.empty() && current[0] == '#')) {
            ++i;
            continue;
        }
        if (words[0] == "halt") {
            state->halted = true;
            break;
        }
        if (words[0] == "hash-threshold" && words.size() >= 2u) {
            state->hashThreshold = std::atoi(words[1].c_str());
            ++i;
            continue;
        }
        if (words[0] == "skipif" && words.size() >= 2u) {
            state->pendingSkipIf.push_back(words[1]);
            ++i;
            continue;
        }
        if (words[0] == "onlyif" && words.size() >= 2u) {
            state->pendingOnlyIf.push_back(words[1]);
            ++i;
            continue;
        }
        if (words[0] == "statement" && words.size() >= 2u) {
            Record record;
            record.kind = Record::Kind::Statement;
            record.line = static_cast<int>(i + 1u);
            record.expectation = words[1];
            record.skipIf = state->pendingSkipIf;
            record.onlyIf = state->pendingOnlyIf;
            record.hashThreshold = state->hashThreshold;
            ++i;
            while (i < lines.size() && !lines[i].empty()) {
                if (!record.sql.empty())
                    record.sql += '\n';
                record.sql += lines[i];
                ++i;
            }
            records.push_back(std::move(record));
            state->pendingOnlyIf.clear();
            state->pendingSkipIf.clear();
            continue;
        }
        if (words[0] == "query" && words.size() >= 2u) {
            Record record;
            record.kind = Record::Kind::Query;
            record.line = static_cast<int>(i + 1u);
            record.types = words[1];
            record.skipIf = state->pendingSkipIf;
            record.onlyIf = state->pendingOnlyIf;
            record.hashThreshold = state->hashThreshold;
            if (words.size() >= 3u &&
                (words[2] == "nosort" || words[2] == "rowsort" || words[2] == "valuesort")) {
                record.sortMode = words[2];
                if (words.size() >= 4u)
                    record.label = words[3];
            } else if (words.size() >= 3u) {
                record.label = words[2];
            }
            ++i;
            while (i < lines.size() && lines[i] != "----") {
                if (!record.sql.empty())
                    record.sql += '\n';
                record.sql += lines[i];
                ++i;
            }
            if (i < lines.size() && lines[i] == "----")
                ++i;
            while (i < lines.size() && !lines[i].empty()) {
                record.expected.push_back(lines[i]);
                ++i;
            }
            records.push_back(std::move(record));
            state->pendingOnlyIf.clear();
            state->pendingSkipIf.clear();
            continue;
        }
        ++i;
    }
    return records;
}

std::unique_ptr<Engine> makeEngine(const std::string& engine, const std::filesystem::path& tempDir,
                                   const std::filesystem::path& file)
{
    if (engine == "sqlite")
        return std::unique_ptr<Engine>(new SqliteEngine());
    const std::filesystem::path dbPath =
        tempDir / (file.filename().string() + "-" + std::to_string(std::time(nullptr)) + ".sqldb");
    return std::unique_ptr<Engine>(new LibsqlEngine(dbPath));
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

bool runRecord(Engine& engine, const Record& record, int hashThreshold, bool verbose,
               const std::filesystem::path& file)
{
    const ExecResult result = engine.exec(record.sql);
    bool ok = false;
    std::vector<std::string> actual;
    std::string message;

    if (record.kind == Record::Kind::Statement) {
        if (record.expectation == "ok")
            ok = result.ok;
        else
            ok = !result.ok;
        if (!ok) {
            message = result.ok ? "statement succeeded unexpectedly" :
                                  ("statement failed unexpectedly: " + result.error);
        }
    } else {
        if (!result.ok) {
            message = "query failed: " + result.error;
        } else if (result.result.columns.size() != record.types.size()) {
            std::ostringstream out;
            out << "column count mismatch: expected " << record.types.size() << ", got "
                << result.result.columns.size();
            message = out.str();
        } else {
            actual = conformance::sltRenderedValues(result.result, record.types);
            conformance::applySltSort(actual, record.sortMode, record.types.size());
            actual = conformance::sltExpectedShape(actual, hashThreshold);
            ok = (actual == record.expected);
            if (!ok)
                message = "result mismatch";
        }
    }

    if (!ok && verbose) {
        std::cerr << file << ':' << record.line << ": " << message << '\n'
                  << "SQL:\n"
                  << record.sql << "\nexpected:\n"
                  << joinLines(record.expected) << "actual:\n"
                  << joinLines(actual);
        if (actual.empty())
            std::cerr << "(no rows)\n";
        std::cerr << '\n';
    }
    return ok;
}

FileStats runFile(const Options& options, const std::filesystem::path& file,
                  const std::vector<SkipEntry>& skipEntries, int* totalFailures)
{
    FileStats stats;
    stats.path = file.string();
    ParseState parse;
    const std::vector<Record> records = parseFile(file, &parse);
    const std::filesystem::path tempDir = conformance::makeTempDir("libsql-slt");
    std::unique_ptr<Engine> engine = makeEngine(options.engine, tempDir, file);

    for (const Record& record : records) {
        if (options.maxFailures >= 0 && *totalFailures >= options.maxFailures)
            break;

        std::string skipReason;
        const bool skippedByList = skipListMatches(skipEntries, file, record.line, &skipReason);
        const bool skippedByCondition = conditionApplies(record.skipIf) ||
                                        !onlyIfAllows(record.onlyIf);
        if (skippedByList || skippedByCondition) {
            ++stats.skipped;
            if (options.verbose) {
                std::cerr << file << ':' << record.line << ": skipped";
                if (!skipReason.empty())
                    std::cerr << " (" << skipReason << ')';
                std::cerr << '\n';
            }
            continue;
        }

        ++stats.run;
        if (runRecord(*engine, record, record.hashThreshold, options.verbose, file)) {
            ++stats.passed;
        } else {
            ++stats.failed;
            ++(*totalFailures);
            if (!options.verbose)
                std::cerr << file << ':' << record.line << ": unexpected failure\n";
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::cout << file << ": run " << stats.run << ", passed " << stats.passed << ", failed "
              << stats.failed << ", skipped " << stats.skipped << '\n';
    return stats;
}

void writeSummaryJson(const std::filesystem::path& path, const std::vector<FileStats>& files)
{
    int run = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const FileStats& stats : files) {
        run += stats.run;
        passed += stats.passed;
        failed += stats.failed;
        skipped += stats.skipped;
    }

    std::ostringstream out;
    out << "{\n  \"total\": {\"run\": " << run << ", \"passed\": " << passed
        << ", \"failed\": " << failed << ", \"skipped\": " << skipped << "},\n"
        << "  \"files\": [\n";
    for (std::size_t i = 0; i < files.size(); ++i) {
        const FileStats& f = files[i];
        out << "    {\"path\": \"" << conformance::jsonEscape(f.path) << "\", \"run\": " << f.run
            << ", \"passed\": " << f.passed << ", \"failed\": " << f.failed
            << ", \"skipped\": " << f.skipped << '}';
        if (i + 1u != files.size())
            out << ',';
        out << '\n';
    }
    out << "  ]\n}\n";
    conformance::writeFile(path, out.str());
}

Options parseArgs(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--engine" && i + 1 < argc) {
            options.engine = argv[++i];
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--max-failures" && i + 1 < argc) {
            options.maxFailures = std::atoi(argv[++i]);
        } else if (arg == "--skip-list" && i + 1 < argc) {
            options.skipList = argv[++i];
        } else if (arg == "--summary-json" && i + 1 < argc) {
            options.summaryJson = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option " + arg);
        } else {
            options.inputs.push_back(arg);
        }
    }
    if (options.engine != "libsql" && options.engine != "sqlite")
        throw std::runtime_error("--engine must be libsql or sqlite");
    if (options.inputs.empty())
        throw std::runtime_error("no input files or directories");
    return options;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseArgs(argc, argv);
        const std::vector<SkipEntry> skipEntries = loadSkipList(options.skipList);
        const std::vector<std::filesystem::path> files =
            conformance::collectTestFiles(options.inputs, ".test");
        if (files.empty())
            throw std::runtime_error("no .test files found");

        std::vector<FileStats> summaries;
        int totalFailures = 0;
        for (const std::filesystem::path& file : files) {
            if (options.maxFailures >= 0 && totalFailures >= options.maxFailures)
                break;
            summaries.push_back(runFile(options, file, skipEntries, &totalFailures));
        }

        int run = 0;
        int passed = 0;
        int failed = 0;
        int skipped = 0;
        for (const FileStats& stats : summaries) {
            run += stats.run;
            passed += stats.passed;
            failed += stats.failed;
            skipped += stats.skipped;
        }
        std::cout << "TOTAL: run " << run << ", passed " << passed << ", failed " << failed
                  << ", skipped " << skipped << '\n';
        if (!options.summaryJson.empty())
            writeSummaryJson(options.summaryJson, summaries);
        return failed == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "slt_runner: " << e.what() << '\n';
        return 2;
    }
}
