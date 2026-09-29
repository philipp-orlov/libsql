// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// sql -- command line front end for a libsql database.
//
//   sql <database> [-c SQL] [-f FILE] [options]
//
// With no -c and no -f it reads statements from stdin, which makes it both a
// prompt to poke at a database with and a pipe to feed a schema through.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define SQL_ISATTY _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define SQL_ISATTY isatty(fileno(stdin))
#endif

#include "sql/sql.hpp"

namespace {

using namespace sql;

constexpr const char* kUsage =
    "sql -- libsql command line\n"
    "\n"
    "usage:\n"
    "  sql <database> [options]\n"
    "\n"
    "options:\n"
    "  -c SQL        run SQL, then exit; may be repeated\n"
    "  -f FILE       run the statements in FILE; may be repeated\n"
    "  -i            keep reading stdin after any -c or -f\n"
    "  --mode M      output as table (default), csv or list\n"
    "  --no-header   leave the column names out\n"
    "  --readonly    open an existing database and refuse to write to it\n"
    "  -h, --help    this text\n"
    "\n"
    "dot commands, when reading statements from stdin:\n"
    "  .tables               names of the tables\n"
    "  .schema [TABLE]       the CREATE statements that would rebuild it\n"
    "  .indexes [TABLE]      indexes, with the columns they cover\n"
    "  .mode table|csv|list  change the output format\n"
    "  .headers on|off       show or hide the column names\n"
    "  .help                 this text\n"
    "  .quit                 leave\n"
    "\n"
    "exit status: 0 ok, 1 bad usage, 2 statement or store error\n";

constexpr int kOk = 0;
constexpr int kUsageError = 1;
constexpr int kStatementError = 2;

enum class Mode
{
    Table,
    Csv,
    List,
};

struct Settings
{
    Mode mode = Mode::Table;
    bool headers = true;
};

// ------------------------------------------------------------- printing ---

std::string cell(const Value& v, Mode mode)
{
    if (v.isNull())
        return mode == Mode::Table ? "NULL" : "";
    return v.toText();
}

std::string csvField(const std::string& s)
{
    if (s.find_first_of(",\"\n\r") == std::string::npos)
        return s;
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    return out + "\"";
}

void printSeparated(const Result& r, const Settings& settings, char sep, bool quote)
{
    auto emit = [&](const std::string& s) { return quote ? csvField(s) : s; };
    if (settings.headers) {
        for (std::size_t i = 0; i < r.columns().size(); ++i) {
            if (i)
                std::putchar(sep);
            std::fputs(emit(r.columns()[i]).c_str(), stdout);
        }
        std::putchar('\n');
    }
    for (const Row& row : r) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i)
                std::putchar(sep);
            std::fputs(emit(cell(row[i], settings.mode)).c_str(), stdout);
        }
        std::putchar('\n');
    }
}

void printTable(const Result& r, const Settings& settings)
{
    const std::size_t columns = r.columns().size();
    std::vector<std::size_t> width(columns, 0);
    for (std::size_t i = 0; i < columns; ++i)
        width[i] = settings.headers ? r.columns()[i].size() : 0;

    std::vector<std::vector<std::string>> text;
    text.reserve(r.size());
    for (const Row& row : r) {
        std::vector<std::string> line;
        line.reserve(columns);
        for (std::size_t i = 0; i < columns && i < row.size(); ++i) {
            line.push_back(cell(row[i], settings.mode));
            width[i] = std::max(width[i], line.back().size());
        }
        text.push_back(std::move(line));
    }

    auto rule = [&] {
        std::putchar('+');
        for (std::size_t i = 0; i < columns; ++i) {
            for (std::size_t k = 0; k < width[i] + 2; ++k)
                std::putchar('-');
            std::putchar('+');
        }
        std::putchar('\n');
    };
    auto line = [&](const std::vector<std::string>& fields) {
        std::putchar('|');
        for (std::size_t i = 0; i < columns; ++i) {
            const std::string& s = i < fields.size() ? fields[i] : std::string();
            std::printf(" %-*s |", int(width[i]), s.c_str());
        }
        std::putchar('\n');
    };

    if (columns == 0)
        return;
    rule();
    if (settings.headers) {
        line(r.columns());
        rule();
    }
    for (const auto& row : text)
        line(row);
    rule();
}

void report(const Result& r, const Settings& settings)
{
    if (!r.columns().empty()) {
        switch (settings.mode) {
            case Mode::Table: printTable(r, settings); break;
            case Mode::Csv: printSeparated(r, settings, ',', true); break;
            case Mode::List: printSeparated(r, settings, '|', false); break;
        }
        std::printf("%zu row%s\n", r.size(), r.size() == 1 ? "" : "s");
    } else if (r.changes() != 0) {
        std::printf("%llu row%s changed\n", (unsigned long long)r.changes(),
                    r.changes() == 1 ? "" : "s");
    }
}

// -------------------------------------------------------- dot commands ----

std::string quoteName(const std::string& name)
{
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_';
        if (!plain)
            return "\"" + name + "\"";
    }
    return name;
}

void printSchema(Database& db, const std::string& only)
{
    for (const std::string& name : db.tables()) {
        if (!only.empty() && name != only)
            continue;
        const TableInfo info = db.table(name);
        // A one-column key is printed inline; a composite one as a table
        // constraint after the columns, the only way SQL can spell it.
        const bool composite = info.primaryKey.size() > 1;
        std::printf("CREATE TABLE %s (\n", quoteName(info.name).c_str());
        for (std::size_t i = 0; i < info.columns.size(); ++i) {
            const ColumnInfo& c = info.columns[i];
            std::printf("  %s %s", quoteName(c.name).c_str(), toString(c.type));
            if (c.primaryKey && !composite)
                std::printf(" PRIMARY KEY");
            else if (c.notNull)
                std::printf(" NOT NULL");
            if (c.unique)
                std::printf(" UNIQUE");
            if (c.hasDefault)
                std::printf(" DEFAULT %s", c.defaultValue.toLiteral().c_str());
            std::printf("%s\n", i + 1 < info.columns.size() || composite ? "," : "");
        }
        if (composite) {
            std::printf("  PRIMARY KEY (");
            for (std::size_t i = 0; i < info.primaryKey.size(); ++i) {
                const ColumnInfo& c = info.columns[std::size_t(info.primaryKey[i])];
                std::printf("%s%s", i ? ", " : "", quoteName(c.name).c_str());
            }
            std::printf(")\n");
        }
        std::printf(");\n");
        for (const IndexInfo& ix : db.indexes(name)) {
            std::printf("CREATE %sINDEX %s ON %s (", ix.unique ? "UNIQUE " : "",
                        quoteName(ix.name).c_str(), quoteName(ix.table).c_str());
            for (std::size_t i = 0; i < ix.columns.size(); ++i)
                std::printf("%s%s", i ? ", " : "", quoteName(ix.columns[i]).c_str());
            std::printf(");\n");
        }
    }
}

/// Returns false when the command asked to quit.
bool dotCommand(Database& db, Settings& settings, std::string_view line)
{
    std::vector<std::string> words;
    for (std::size_t i = 0; i < line.size();) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i > start)
            words.emplace_back(line.substr(start, i - start));
    }
    if (words.empty())
        return true;

    const std::string& command = words[0];
    const std::string argument = words.size() > 1 ? words[1] : std::string();

    if (command == ".quit" || command == ".exit")
        return false;
    if (command == ".help") {
        std::fputs(kUsage, stdout);
    } else if (command == ".tables") {
        for (const std::string& name : db.tables())
            std::printf("%s\n", name.c_str());
    } else if (command == ".schema") {
        printSchema(db, argument);
    } else if (command == ".indexes") {
        for (const IndexInfo& ix : db.indexes(argument)) {
            std::printf("%s on %s (", ix.name.c_str(), ix.table.c_str());
            for (std::size_t i = 0; i < ix.columns.size(); ++i)
                std::printf("%s%s", i ? ", " : "", ix.columns[i].c_str());
            std::printf(")%s\n", ix.unique ? " unique" : "");
        }
    } else if (command == ".mode") {
        if (argument == "table")
            settings.mode = Mode::Table;
        else if (argument == "csv")
            settings.mode = Mode::Csv;
        else if (argument == "list")
            settings.mode = Mode::List;
        else
            std::fprintf(stderr, "sql: .mode wants table, csv or list\n");
    } else if (command == ".headers") {
        settings.headers = argument != "off";
    } else {
        std::fprintf(stderr, "sql: unknown command '%s'; try .help\n", command.c_str());
    }
    return true;
}

// --------------------------------------------------------------- input ----

/// True once the buffer holds a statement terminator that is not inside a
/// literal or a comment, so the prompt knows when to stop collecting lines.
bool complete(std::string_view text)
{
    bool terminated = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\'' || c == '"' || c == '`') {
            const char close = c;
            for (++i; i < text.size() && text[i] != close; ++i) {
            }
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/'))
                ++i;
            ++i;
            continue;
        }
        if (c == ';')
            terminated = true;
        else if (!std::isspace(static_cast<unsigned char>(c)))
            terminated = false;
    }
    return terminated;
}

bool blank(std::string_view text)
{
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

/// Runs one batch and prints whatever it produced. Returns false on error.
bool run(Database& db, const Settings& settings, const std::string& text)
{
    try {
        report(db.exec(text), settings);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sql: %s\n", e.what());
        return false;
    }
}

bool readAll(const char* path, std::string& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    char buffer[8192];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
        out.append(buffer, n);
    std::fclose(f);
    return true;
}

int prompt(Database& db, Settings& settings)
{
    const bool tty = SQL_ISATTY != 0;
    std::string pending;
    char line[4096];

    if (tty)
        std::printf("libsql %s -- .help for commands, .quit to leave\n", version());
    for (;;) {
        if (tty) {
            std::printf("%s ", pending.empty() ? "sql>" : "...>");
            std::fflush(stdout);
        }
        if (!std::fgets(line, sizeof line, stdin))
            break;

        if (pending.empty() && line[0] == '.') {
            if (!dotCommand(db, settings, line))
                break;
            continue;
        }
        pending += line;
        if (!complete(pending))
            continue;
        if (!blank(pending))
            run(db, settings, pending);
        pending.clear();
    }
    if (!blank(pending))
        run(db, settings, pending);
    return kOk;
}

}  // namespace

int main(int argc, char** argv)
{
    Settings settings;
    std::string path;
    std::vector<std::string> batches;
    bool interactive = false;
    bool readOnly = false;
    bool modeGiven = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sql: %s needs a value\n", flag);
                std::exit(kUsageError);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stdout);
            return kOk;
        } else if (arg == "-c") {
            batches.emplace_back(value("-c"));
        } else if (arg == "-f") {
            const char* file = value("-f");
            std::string text;
            if (!readAll(file, text)) {
                std::fprintf(stderr, "sql: cannot read %s\n", file);
                return kUsageError;
            }
            batches.push_back(std::move(text));
        } else if (arg == "-i") {
            interactive = true;
        } else if (arg == "--mode") {
            const std::string_view mode = value("--mode");
            modeGiven = true;
            if (mode == "table")
                settings.mode = Mode::Table;
            else if (mode == "csv")
                settings.mode = Mode::Csv;
            else if (mode == "list")
                settings.mode = Mode::List;
            else {
                std::fprintf(stderr, "sql: --mode wants table, csv or list\n");
                return kUsageError;
            }
        } else if (arg == "--no-header") {
            settings.headers = false;
        } else if (arg == "--readonly") {
            readOnly = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "sql: unknown option: %s\n\n", argv[i]);
            std::fputs(kUsage, stderr);
            return kUsageError;
        } else if (path.empty()) {
            path = arg;
        } else {
            std::fprintf(stderr, "sql: unexpected argument: %s\n", argv[i]);
            return kUsageError;
        }
    }

    if (path.empty()) {
        std::fputs(kUsage, stderr);
        return kUsageError;
    }
    // Piped batches default to something a script can parse.
    if (!modeGiven && !batches.empty())
        settings.mode = Mode::List;

    Database db;
    try {
        db = Database::configure().readOnly(readOnly).createIfMissing(!readOnly).open(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sql: %s\n", e.what());
        return kStatementError;
    }

    int status = kOk;
    for (const std::string& batch : batches) {
        if (!run(db, settings, batch))
            status = kStatementError;
    }
    if (batches.empty() || interactive)
        prompt(db, settings);
    return status;
}
