// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Numbers in SQL text, values and JSON must not depend on the host's
// LC_NUMERIC: a locale with a decimal comma may not change what "1.5" means
// or how 1.5 is written.

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "sql/json.hpp"
#include "sql_test.hpp"

using namespace sql;

namespace {
/// Switches LC_NUMERIC to a decimal-comma locale for its lifetime. Uses one the
/// system has; failing that, compiles de_DE with localedef into a temporary
/// directory. Not available at all: `active()` is false and the test does nothing.
class CommaLocale
{
public:
    CommaLocale()
    {
        previous_ = setlocale(LC_NUMERIC, nullptr) ? setlocale(LC_NUMERIC, nullptr) : "C";
        for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "ru_RU.UTF-8"})
            if (try_(name))
                return;

        dir_ = std::filesystem::temp_directory_path() / ("libsql-locale-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
        const std::string command = "localedef -i de_DE -f UTF-8 '" + (dir_ / "de_DE.UTF-8").string() +
                                    "' >/dev/null 2>&1";
        if (std::system(command.c_str()) != 0 && !std::filesystem::exists(dir_ / "de_DE.UTF-8"))
            return;

        setenv("LOCPATH", dir_.c_str(), 1);
        try_("de_DE.UTF-8");
    }
    ~CommaLocale()
    {
        setlocale(LC_NUMERIC, previous_.c_str());
        if (!dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
        }
    }
    bool active() const { return active_; }

private:
    bool try_(const char* name)
    {
        if (!setlocale(LC_NUMERIC, name))
            return false;

        char probe[16];
        std::snprintf(probe, sizeof probe, "%.1f", 1.5);
        active_ = std::string(probe) == "1,5";
        if (!active_)
            setlocale(LC_NUMERIC, previous_.c_str());

        return active_;
    }

    std::string previous_;
    std::filesystem::path dir_;
    bool active_ = false;
};
}  // namespace

TEST(numbersIgnoreTheHostLocale)
{
    tst::Scratch s("locale");
    Database db = s.open();
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, r REAL)");

    CommaLocale locale;
    if (!locale.active()) {
        std::puts("  (no decimal-comma locale available; skipped)");
        return;
    }
    // Literals with a fraction or exponent read the same as in the C locale.
    db.exec("INSERT INTO t (r) VALUES ('2.75')");
    CHECK_EQ(tst::only(db.exec("SELECT 1.5 + 1 FROM t")), std::string("2.5"));
    CHECK_EQ(tst::only(db.exec("SELECT 2.5e2 FROM t")), std::string("250"));
    // Text coerced into a REAL column.
    CHECK_EQ(tst::only(db.exec("SELECT r FROM t")), std::string("2.75"));
    // Values render with a point, in text and in JSON.
    CHECK_EQ(Value(0.1).toText(), std::string("0.1"));
    CHECK_EQ(Value(1234.5).toText(), std::string("1234.5"));
    const std::string json = toJson(db.exec("SELECT r FROM t"));
    CHECK(json.find("2.75") != std::string::npos);
    CHECK(json.find("2,75") == std::string::npos);
}

int main()
{
    return tst::runAll("locale");
}
