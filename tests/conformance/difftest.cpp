// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "conformance_util.hpp"
#include "sqlgen.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>

using conformance::ExecResult;
using conformance::LibsqlDb;
using conformance::QueryResult;
using conformance::Row;
using conformance::SqliteDb;

namespace {

struct Options
{
    bool fuzz = false;
    std::uint64_t seed = 1;
    int count = 0;
    std::filesystem::path outDir;
    std::vector<std::string> scripts;
};

struct Comparison
{
    bool hardMismatch = false;
    bool softNameMismatch = false;
    /// ORDER BY ties came out in a different order: the same rows, and equal
    /// position by position except that an INTEGER faced an equal REAL (1 vs
    /// 1.0 compare equal, so either order is correct).
    bool tieOrder = false;
    std::string report;
};

class PairRunner
{
public:
    explicit PairRunner(const std::filesystem::path& tempDir)
        : libsql_(tempDir / "libsql.sqldb")
    {}

    Comparison runAndCompare(const std::string& sql)
    {
        const ExecResult sqlite = sqlite_.exec(sql);
        const ExecResult libsql = libsql_.exec(sql);
        return compare(sql, sqlite, libsql);
    }

    void execPrefix(const std::vector<std::string>& statements)
    {
        for (const std::string& statement : statements) {
            (void)sqlite_.exec(statement);
            (void)libsql_.exec(statement);
        }
    }

private:
    static bool looseCell(const conformance::Cell& a, const conformance::Cell& b)
    {
        using conformance::CellType;
        if (conformance::sameCell(a, b))
            return true;
        const bool an = a.type == CellType::Integer || a.type == CellType::Real;
        const bool bn = b.type == CellType::Integer || b.type == CellType::Real;
        if (!an || !bn || a.type == b.type)
            return false;
        const double x = a.type == CellType::Integer ? double(a.integer) : a.real;
        const double y = b.type == CellType::Integer ? double(b.integer) : b.real;
        return x == y;
    }

    /// Not identical, but the same multiset of rows and equal position by
    /// position up to INTEGER/REAL ties.
    static bool sameRowsUpToNumericTies(const std::vector<conformance::Row>& a,
                                        const std::vector<conformance::Row>& b)
    {
        bool identical = true;
        for (std::size_t r = 0; r < a.size(); ++r) {
            if (a[r].size() != b[r].size())
                return false;
            for (std::size_t c = 0; c < a[r].size(); ++c) {
                if (!looseCell(a[r][c], b[r][c]))
                    return false;
                identical = identical && conformance::sameCell(a[r][c], b[r][c]);
            }
        }
        if (identical)
            return false;
        std::vector<conformance::Row> x = a, y = b;
        std::sort(x.begin(), x.end(), conformance::rowLess);
        std::sort(y.begin(), y.end(), conformance::rowLess);
        for (std::size_t r = 0; r < x.size(); ++r) {
            for (std::size_t c = 0; c < x[r].size(); ++c) {
                if (!conformance::sameCell(x[r][c], y[r][c]))
                    return false;
            }
        }
        return true;
    }

    static Comparison compare(const std::string& sql, ExecResult sqlite, ExecResult libsql)
    {
        Comparison out;
        if (!sqlite.ok && !libsql.ok)
            return out;
        if (sqlite.ok != libsql.ok) {
            out.hardMismatch = true;
            std::ostringstream msg;
            msg << "error mismatch\n  SQLite: "
                << (sqlite.ok ? "ok" : ("error: " + sqlite.error)) << "\n  libsql: "
                << (libsql.ok ? "ok" : ("error: " + libsql.error));
            out.report = msg.str();
            return out;
        }

        QueryResult left = std::move(sqlite.result);
        QueryResult right = std::move(libsql.result);
        if (left.columns.size() != right.columns.size()) {
            out.hardMismatch = true;
            std::ostringstream msg;
            msg << "column count mismatch: SQLite " << left.columns.size() << ", libsql "
                << right.columns.size();
            out.report = msg.str();
            return out;
        }

        for (std::size_t i = 0; i < left.columns.size(); ++i) {
            if (left.columns[i] != right.columns[i]) {
                out.softNameMismatch = true;
                std::ostringstream msg;
                msg << "column name mismatch at " << i << ": SQLite '" << left.columns[i]
                    << "', libsql '" << right.columns[i] << "'";
                out.report = msg.str();
                break;
            }
        }

        if (conformance::isTopLevelSelectWithoutOrderBy(sql)) {
            std::sort(left.rows.begin(), left.rows.end(), conformance::rowLess);
            std::sort(right.rows.begin(), right.rows.end(), conformance::rowLess);
        }

        if (left.rows.size() != right.rows.size()) {
            out.hardMismatch = true;
            std::ostringstream msg;
            msg << "row count mismatch: SQLite " << left.rows.size() << ", libsql "
                << right.rows.size();
            if (!out.report.empty())
                msg << "\n" << out.report;
            out.report = msg.str();
            return out;
        }

        if (sameRowsUpToNumericTies(left.rows, right.rows)) {
            out.tieOrder = true;
            return out;
        }
        for (std::size_t r = 0; r < left.rows.size(); ++r) {
            if (left.rows[r].size() != right.rows[r].size()) {
                out.hardMismatch = true;
                std::ostringstream msg;
                msg << "row width mismatch at row " << r << ": SQLite " << left.rows[r].size()
                    << ", libsql " << right.rows[r].size();
                out.report = msg.str();
                return out;
            }
            for (std::size_t c = 0; c < left.rows[r].size(); ++c) {
                if (!conformance::sameCell(left.rows[r][c], right.rows[r][c])) {
                    out.hardMismatch = true;
                    std::ostringstream msg;
                    msg << "value mismatch at row " << r << ", column " << c << "\n  SQLite: "
                        << conformance::cellDebug(left.rows[r][c])
                        << "\n  libsql: " << conformance::cellDebug(right.rows[r][c]);
                    if (!out.report.empty())
                        msg << "\n" << out.report;
                    out.report = msg.str();
                    return out;
                }
            }
        }
        return out;
    }

    SqliteDb sqlite_;
    LibsqlDb libsql_;
};

Options parseArgs(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fuzz") {
            options.fuzz = true;
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--count" && i + 1 < argc) {
            options.count = std::atoi(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            options.outDir = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option " + arg);
        } else {
            options.scripts.push_back(arg);
        }
    }
    if (options.fuzz && options.count <= 0)
        throw std::runtime_error("--fuzz requires --count N");
    if (!options.fuzz && options.scripts.empty())
        throw std::runtime_error("provide script.sql files or --fuzz");
    return options;
}

std::string formatScript(const std::vector<std::string>& statements)
{
    std::string out;
    for (const std::string& statement : statements) {
        out += statement;
        if (!statement.empty() && statement.find_last_not_of(" \t\r\n") != std::string::npos) {
            const std::size_t last = statement.find_last_not_of(" \t\r\n");
            if (statement[last] != ';')
                out += ';';
        }
        out += "\n\n";
    }
    return out;
}

bool reproducesMismatch(const std::vector<std::string>& statements)
{
    if (statements.empty())
        return false;
    const std::filesystem::path temp = conformance::makeTempDir("libsql-diff-min");
    PairRunner runner(temp);
    std::vector<std::string> prefix(statements.begin(), statements.end() - 1);
    runner.execPrefix(prefix);
    const Comparison cmp = runner.runAndCompare(statements.back());
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    return cmp.hardMismatch;
}

std::vector<std::string> minimize(std::vector<std::string> statements)
{
    if (statements.size() <= 1u)
        return statements;
    for (std::size_t i = 0; i + 1u < statements.size();) {
        std::vector<std::string> candidate = statements;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
        if (reproducesMismatch(candidate)) {
            statements = std::move(candidate);
        } else {
            ++i;
        }
    }
    return statements;
}

void writeRepro(const std::filesystem::path& outDir, int index, const std::vector<std::string>& statements)
{
    if (outDir.empty())
        return;
    const std::vector<std::string> minimized = minimize(statements);
    std::ostringstream name;
    name << "repro-" << index << ".sql";
    conformance::writeFile(outDir / name.str(), formatScript(minimized));
}

int runScripts(const Options& options)
{
    int mismatches = 0;
    int softNames = 0;
    for (const std::string& scriptPath : options.scripts) {
        const std::filesystem::path temp = conformance::makeTempDir("libsql-diff");
        PairRunner runner(temp);
        const std::vector<std::string> statements =
            conformance::splitSqlScript(conformance::readFile(scriptPath));
        std::vector<std::string> prefix;
        int statementNumber = 0;
        for (const std::string& statement : statements) {
            ++statementNumber;
            const Comparison cmp = runner.runAndCompare(statement);
            if (cmp.softNameMismatch) {
                ++softNames;
                std::cout << scriptPath << " statement " << statementNumber << ": soft "
                          << cmp.report << '\n';
            }
            if (cmp.hardMismatch) {
                ++mismatches;
                std::cout << scriptPath << " statement " << statementNumber << " mismatch:\n"
                          << cmp.report << "\nSQL:\n"
                          << statement << "\n\n";
                std::vector<std::string> repro = prefix;
                repro.push_back(statement);
                writeRepro(options.outDir, mismatches, repro);
            }
            prefix.push_back(statement);
        }
        std::error_code ec;
        std::filesystem::remove_all(temp, ec);
    }
    std::cout << "difftest: " << mismatches << " hard mismatches, " << softNames
              << " soft column-name mismatches\n";
    return mismatches == 0 ? 0 : 1;
}

int runFuzz(const Options& options)
{
    conformance::SqlGenerator generator(options.seed);
    const std::filesystem::path temp = conformance::makeTempDir("libsql-diff-fuzz");
    PairRunner runner(temp);
    std::vector<std::string> history;
    int mismatches = 0;
    int softNames = 0;
    int ties = 0;

    for (const std::string& setup : generator.setup()) {
        const Comparison cmp = runner.runAndCompare(setup);
        history.push_back(setup);
        if (cmp.hardMismatch) {
            ++mismatches;
            std::cout << "fuzz setup mismatch:\n" << cmp.report << "\nSQL:\n" << setup << "\n\n";
            writeRepro(options.outDir, mismatches, history);
        }
    }

    for (int i = 0; i < options.count; ++i) {
        const conformance::GeneratedStatement generated = generator.next();
        const Comparison cmp = runner.runAndCompare(generated.sql);
        history.push_back(generated.sql);
        if (cmp.softNameMismatch) {
            ++softNames;
            std::cout << "fuzz " << i << ": soft " << cmp.report << '\n';
        }
        ties += cmp.tieOrder;
        if (cmp.hardMismatch) {
            ++mismatches;
            std::cout << "fuzz " << i << " mismatch:\n"
                      << cmp.report << "\nSQL:\n"
                      << generated.sql << "\n\n";
            writeRepro(options.outDir, mismatches, history);
        }
        if (generated.checkState && !generated.table.empty()) {
            const std::string check = generator.stateCheck(generated.table);
            const Comparison stateCmp = runner.runAndCompare(check);
            history.push_back(check);
            if (stateCmp.hardMismatch) {
                ++mismatches;
                std::cout << "fuzz " << i << " state mismatch:\n"
                          << stateCmp.report << "\nSQL:\n"
                          << check << "\n\n";
                writeRepro(options.outDir, mismatches, history);
            }
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::cout << "difftest fuzz: " << mismatches << " hard mismatches, " << softNames
              << " soft column-name mismatches, " << ties << " INTEGER/REAL tie orders\n";
    return mismatches == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseArgs(argc, argv);
        if (options.fuzz)
            return runFuzz(options);
        return runScripts(options);
    } catch (const std::exception& e) {
        std::cerr << "difftest: " << e.what() << '\n';
        return 2;
    }
}
