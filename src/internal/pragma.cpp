// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// PRAGMA statements, the pragma_* table-valued functions, and the schema
// tables sqlite_schema (sqlite_master), sqlite_temp_schema and
// sqlite_sequence.

#include <algorithm>
#include <functional>

#include "internal/dml.hpp"
#include "internal/encoding.hpp"
#include "internal/parser.hpp"
#include "internal/stmt.hpp"
#include "internal/util.hpp"

namespace sql::internal {

namespace {

const char* const kPragmas[] = {
    "analysis_limit",   "application_id",   "auto_vacuum",      "automatic_index",
    "busy_timeout",     "cache_size",       "cache_spill",      "case_sensitive_like",
    "cell_size_check",  "checkpoint_fullfsync", "collation_list", "compile_options",
    "count_changes",    "data_version",     "database_list",    "default_cache_size",
    "defer_foreign_keys", "empty_result_callbacks", "encoding", "foreign_key_check",
    "foreign_key_list", "foreign_keys",     "freelist_count",   "full_column_names",
    "fullfsync",        "function_list",    "hard_heap_limit",  "ignore_check_constraints",
    "incremental_vacuum", "index_info",     "index_list",       "index_xinfo",
    "integrity_check",  "journal_mode",     "journal_size_limit", "legacy_alter_table",
    "locking_mode",     "max_page_count",   "mmap_size",        "module_list",
    "optimize",         "page_count",       "page_size",        "pragma_list",
    "query_only",       "quick_check",      "read_uncommitted", "recursive_triggers",
    "reverse_unordered_selects", "schema_version", "secure_delete", "short_column_names",
    "shrink_memory",    "soft_heap_limit",  "synchronous",      "table_info",
    "table_list",       "table_xinfo",      "temp_store",       "temp_store_directory",
    "threads",          "trusted_schema",   "user_version",     "wal_autocheckpoint",
    "wal_checkpoint",   "writable_schema",
};

bool flagValue(const Value& v)
{
    if (v.type() == Type::Text) {
        const std::string_view s = v.text();
        if (equalsNoCase(s, "on") || equalsNoCase(s, "yes") || equalsNoCase(s, "true"))
            return true;
        if (equalsNoCase(s, "off") || equalsNoCase(s, "no") || equalsNoCase(s, "false") ||
            equalsNoCase(s, "none"))
            return false;
        if (equalsNoCase(s, "full") || equalsNoCase(s, "normal") || equalsNoCase(s, "extra"))
            return true;
    }
    return toInt64(v) != 0;
}

std::string text(const Value& v)
{
    std::string s;
    appendText(v, s);
    return s;
}

const char* actionName(FkAction a)
{
    switch (a) {
        case FkAction::NoAction: return "NO ACTION";
        case FkAction::Restrict: return "RESTRICT";
        case FkAction::SetNull: return "SET NULL";
        case FkAction::SetDefault: return "SET DEFAULT";
        case FkAction::Cascade: return "CASCADE";
    }
    return "NO ACTION";
}

/// Finds a table (any store, or `schema`) for a pragma argument.
const Table* pragmaTable(Session& s, const std::string& schema, const std::string& name, std::size_t& store)
{
    for (std::size_t i = 0; i < s.stores.size(); ++i) {
        const std::size_t k = i == 0 && s.stores.size() > 1 ? 1 : (i == 1 ? 0 : i);
        if (!s.stores[k].catalog)
            continue;
        if (!schema.empty() && !equalsNoCase(s.stores[k].name, schema))
            continue;
        if (const Table* t = s.catalog(k).findTable(name)) {
            store = k;
            return t;
        }
    }
    return nullptr;
}

std::vector<std::string> viewColumns(Session& s, const View& v)
{
    Program probe;
    probe.text = v.sql;
    probe.node = std::make_unique<StatementNode>(parseOne(v.sql));
    auto& def = std::get<CreateViewStmt>(probe.node->body);
    Planner pl(probe, s);
    auto sp = pl.planSelect(*def.select, nullptr, probe.text, nullptr);
    return def.columns.empty() ? sp->subNames : def.columns;
}

void foreignKeyCheck(Session& s, const std::string& only, PragmaResult& r)
{
    for (std::size_t i = 0; i < s.stores.size(); ++i) {
        if (!s.stores[i].catalog)
            continue;
        Catalog& cat = s.catalog(i);
        for (const auto& tp : cat.schema().tables) {
            const Table& t = *tp;
            if (!only.empty() && !equalsNoCase(t.name, only))
                continue;
            for (std::size_t f = 0; f < t.foreignKeys.size(); ++f) {
                const ForeignKey& fk = t.foreignKeys[f];
                std::size_t pstore = 0;
                const Table* parent = pragmaTable(s, "", fk.parentTable, pstore);
                std::vector<int> pcols;
                if (parent) {
                    if (fk.parentColumns.empty()) {
                        pcols = parent->primaryKey;
                        if (pcols.empty())
                            pcols.push_back(int(parent->columns.size()));
                    } else {
                        for (const std::string& n : fk.parentColumns)
                            pcols.push_back(parent->find(n));
                    }
                }
                // Scan the child; probe the parent by scanning (correctness first).
                nosql::Cursor cur = cat.rows(t).cursor();
                std::vector<Value> row(t.width());
                for (bool ok = cur.first(); ok; ok = cur.next()) {
                    std::string_view payload = viewOf(cur.value());
                    std::int64_t rowid = 0;
                    if (t.payloadRowid()) {
                        const std::uint64_t z = getVarint(payload);
                        rowid = std::int64_t((z >> 1) ^ -(z & 1));
                    } else if (t.keyedByRowid()) {
                        std::string_view k = viewOf(cur.key());
                        rowid = readRowidKey(k);
                    }
                    decodeRowInto(payload, row.data(), t.columns.size());
                    if (t.rowidAlias >= 0)
                        row[std::size_t(t.rowidAlias)] = Value(rowid);
                    bool anyNull = false;
                    for (const int c : fk.columns)
                        anyNull = anyNull || row[std::size_t(c)].isNull();
                    if (anyNull)
                        continue;
                    bool found = false;
                    if (parent && pcols.size() == fk.columns.size()) {
                        nosql::Cursor pc = s.catalog(pstore).rows(*parent).cursor();
                        std::vector<Value> prow(parent->width());
                        for (bool pk = pc.first(); pk && !found; pk = pc.next()) {
                            std::string_view pp = viewOf(pc.value());
                            std::int64_t prowid = 0;
                            if (parent->payloadRowid()) {
                                const std::uint64_t z = getVarint(pp);
                                prowid = std::int64_t((z >> 1) ^ -(z & 1));
                            } else if (parent->keyedByRowid()) {
                                std::string_view k = viewOf(pc.key());
                                prowid = readRowidKey(k);
                            }
                            decodeRowInto(pp, prow.data(), parent->columns.size());
                            if (parent->hasRowid())
                                prow[parent->columns.size()] = Value(prowid);
                            if (parent->rowidAlias >= 0)
                                prow[std::size_t(parent->rowidAlias)] = Value(prowid);
                            bool match = true;
                            for (std::size_t k = 0; k < pcols.size() && match; ++k) {
                                if (pcols[k] < 0) {
                                    match = false;
                                    break;
                                }
                                Value cv = row[std::size_t(fk.columns[k])];
                                if (std::size_t(pcols[k]) < parent->columns.size())
                                    applyAffinity(cv, parent->columns[std::size_t(pcols[k])].affinity);
                                match = compareValues(cv, prow[std::size_t(pcols[k])]) == 0;
                            }
                            found = match;
                        }
                    }
                    if (!found)
                        r.rows.push_back({Value(t.name), t.hasRowid() ? Value(rowid) : Value(),
                                          Value(fk.parentTable), Value(std::int64_t(f))});
                }
            }
        }
    }
}

void integrityCheck(Session& s, PragmaResult& r, bool quick)
{
    // Every index must hold exactly the entries its table's rows imply: each
    // row's entry is looked up, then the counts are compared so that stray
    // entries are caught too. quick_check only walks the trees.
    for (std::size_t i = 0; i < s.stores.size(); ++i) {
        if (!s.stores[i].catalog)
            continue;
        Catalog& cat = s.catalog(i);
        for (const auto& owned : cat.schema().tables) {
            const Table& t = *owned;
            (void)cat.rows(t).count();
            if (quick || t.indexes.empty())
                continue;
            Program prog;
            Planner pl(prog, s);
            Writer w(pl, prog);
            TablePlan& tp = w.plan(t, i);
            prog.frame.resize(prog.frameSize);
            FuncContext fctx;
            fctx.conn = s.conn;
            Ctx c;
            c.slots = prog.frame.data();
            c.session = &s;
            c.fctx = &fctx;
            for (const Index* ix : t.indexes) {
                const std::uint64_t expected = w.verifyEntries(tp, c, *ix, [&](std::int64_t row) {
                    if (r.rows.size() < 100)
                        r.rows.push_back({Value("row " + std::to_string(row) + " missing from index " + ix->name)});
                });
                if (cat.entries(*ix).count() != expected && r.rows.size() < 100)
                    r.rows.push_back({Value("wrong # of entries in index " + ix->name)});
            }
        }
    }
    if (r.rows.empty())
        r.rows.push_back({Value("ok")});
}

}  // namespace

std::vector<std::string> pragmaColumns(const std::string& name)
{
    const std::string n = toLower(name);
    if (n == "table_info")
        return {"cid", "name", "type", "notnull", "dflt_value", "pk"};
    if (n == "table_xinfo")
        return {"cid", "name", "type", "notnull", "dflt_value", "pk", "hidden"};
    if (n == "index_list")
        return {"seq", "name", "unique", "origin", "partial"};
    if (n == "index_info")
        return {"seqno", "cid", "name"};
    if (n == "index_xinfo")
        return {"seqno", "cid", "name", "desc", "coll", "key"};
    if (n == "foreign_key_list")
        return {"id", "seq", "table", "from", "to", "on_update", "on_delete", "match"};
    if (n == "table_list")
        return {"schema", "name", "type", "ncol", "wr", "strict"};
    if (n == "database_list")
        return {"seq", "name", "file"};
    if (n == "collation_list")
        return {"seq", "name"};
    if (n == "function_list")
        return {"name", "builtin", "type", "enc", "narg", "flags"};
    if (n == "foreign_key_check")
        return {"table", "rowid", "parent", "fkid"};
    if (n == "wal_checkpoint")
        return {"busy", "log", "checkpointed"};
    if (n == "busy_timeout")
        return {"timeout"};
    if (n == "pragma_list" || n == "module_list")
        return {"name"};
    if (isStoragePragma(n))
        return storagePragmaColumns(n);
    return {n};
}

PragmaResult runPragma(Session& s, const std::string& schema, const std::string& rawName, const Value* value,
                       bool call)
{
    (void)call;
    PragmaResult r;
    const std::string name = toLower(rawName);
    ConnectionState& conn = *s.conn;
    std::size_t storeIdx = 0;
    if (!schema.empty()) {
        const int i = s.storeIndex(schema);
        if (i < 0)
            throw Error(ErrorCode::Runtime, "unknown database " + schema);
        storeIdx = std::size_t(i);
    }
    auto single = [&](Value v) {
        r.columns = pragmaColumns(name);
        r.rows.push_back({std::move(v)});
    };
    auto boolPragma = [&](bool& flag, bool allowSet = true) {
        if (value) {
            if (allowSet)
                flag = flagValue(*value);
            return;
        }
        single(Value(std::int64_t(flag)));
    };
    const std::string arg = value ? text(*value) : std::string();

    if (name == "foreign_keys") {
        if (value && s.explicitTxn)
            return r;  // a no-op inside a transaction, as in SQLite
        boolPragma(conn.foreignKeys);
        return r;
    }
    if (name == "recursive_triggers") {
        boolPragma(conn.recursiveTriggers);
        return r;
    }
    if (name == "case_sensitive_like") {
        if (value)
            conn.caseSensitiveLike = flagValue(*value);
        return r;
    }
    if (name == "defer_foreign_keys") {
        boolPragma(conn.deferForeignKeys);
        return r;
    }
    if (name == "ignore_check_constraints") {
        boolPragma(conn.ignoreCheckConstraints);
        return r;
    }
    if (name == "query_only") {
        boolPragma(conn.queryOnly);
        return r;
    }
    if (name == "reverse_unordered_selects") {
        boolPragma(conn.reverseUnorderedSelects);
        return r;
    }
    if (name == "legacy_alter_table") {
        boolPragma(conn.legacyAlterTable);
        return r;
    }
    if (name == "busy_timeout") {
        if (value)
            conn.busyTimeout = std::max<std::int64_t>(0, toInt64(*value));
        single(Value(conn.busyTimeout.load()));
        return r;
    }
    if (name == "cache_size" || name == "default_cache_size") {
        if (value)
            conn.cacheSize = toInt64(*value);
        else
            single(Value(conn.cacheSize));
        return r;
    }
    if (name == "user_version" || name == "application_id") {
        if (!s.stores[storeIdx].catalog) {
            single(Value(std::int64_t(0)));
            return r;
        }
        Catalog& cat = s.catalog(storeIdx);
        if (value) {
            if (cat.readOnly())
                throw Error(ErrorCode::ReadOnly, "attempt to write a readonly database");
            const std::int64_t v = std::int64_t(std::int32_t(toInt64(*value)));
            if (name == "user_version")
                cat.setUserVersion(v);
            else
                cat.setApplicationId(v);
            return r;
        }
        single(Value(name == "user_version" ? cat.schema().userVersion : cat.schema().applicationId));
        return r;
    }
    if (name == "schema_version") {
        if (value)
            throw Error(ErrorCode::Unsupported, "PRAGMA schema_version cannot be set");
        single(Value(std::int64_t(s.stores[storeIdx].catalog ? s.catalog(storeIdx).schema().version : 0)));
        return r;
    }
    if (name == "data_version") {
        single(Value(std::int64_t(s.stores[storeIdx].env ? s.stores[storeIdx].env->commitGeneration() + 1 : 1)));
        return r;
    }
    if (name == "encoding") {
        single(Value("UTF-8"));
        return r;
    }
    if (name == "journal_mode") {
        single(Value("wal"));
        return r;
    }
    if (name == "locking_mode") {
        single(Value("normal"));
        return r;
    }
    if (name == "synchronous") {
        if (!value)
            single(Value(std::int64_t(2)));
        return r;
    }
    if (name == "page_size" || name == "page_count" || name == "freelist_count" || name == "max_page_count" ||
        name == "mmap_size") {
        if (value && name != "max_page_count" && name != "mmap_size")
            return r;
        nosql::Env* env = s.stores[storeIdx].env;
        if (!env) {
            single(Value(std::int64_t(0)));
            return r;
        }
        const nosql::EnvStats st = env->stats();
        std::int64_t v = 0;
        if (name == "page_size")
            v = st.pageSize;
        else if (name == "page_count")
            v = std::int64_t(st.usedPages);
        else if (name == "freelist_count")
            v = std::int64_t(st.freePages);
        else if (name == "max_page_count")
            v = st.pageSize ? std::int64_t(st.mapSize / st.pageSize) : 0;
        else
            v = std::int64_t(st.mapSize);
        single(Value(v));
        return r;
    }
    if (name == "temp_store") {
        if (value) {
            int v = -1;
            if (value->type() == Type::Text) {
                const std::string_view t = value->text();
                if (equalsNoCase(t, "default"))
                    v = 0;
                else if (equalsNoCase(t, "file"))
                    v = 1;
                else if (equalsNoCase(t, "memory"))
                    v = 2;
            }
            if (v < 0) {
                const std::int64_t n = toInt64(*value);
                v = n >= 0 && n <= 2 ? int(n) : 0;
            }
            conn.tempStore = v;
        } else {
            single(Value(std::int64_t(conn.tempStore.load())));
        }
        return r;
    }
    if (name == "auto_vacuum" || name == "secure_delete" ||
        name == "cell_size_check" || name == "checkpoint_fullfsync" || name == "fullfsync" ||
        name == "count_changes" || name == "empty_result_callbacks" || name == "full_column_names" ||
        name == "read_uncommitted" || name == "writable_schema") {
        if (name == "writable_schema" && value && flagValue(*value))
            throw Error(ErrorCode::Unsupported, "PRAGMA writable_schema is not supported");
        if (!value)
            single(Value(std::int64_t(0)));
        return r;
    }
    if (name == "automatic_index" || name == "short_column_names" || name == "trusted_schema" ||
        name == "cache_spill") {
        if (!value)
            single(Value(std::int64_t(1)));
        return r;
    }
    if (name == "journal_size_limit" || name == "soft_heap_limit" || name == "hard_heap_limit" ||
        name == "threads" || name == "wal_autocheckpoint" || name == "analysis_limit") {
        if (!value)
            single(Value(std::int64_t(name == "journal_size_limit" ? -1 : (name == "wal_autocheckpoint" ? 1000 : 0))));
        return r;
    }
    if (name == "temp_store_directory" || name == "incremental_vacuum" || name == "shrink_memory")
        return r;
    if (name == "wal_checkpoint") {
        r.columns = pragmaColumns(name);
        r.rows.push_back({Value(std::int64_t(0)), Value(std::int64_t(0)), Value(std::int64_t(0))});
        return r;
    }
    if (name == "optimize") {
        return r;
    }
    if (name == "integrity_check" || name == "quick_check") {
        r.columns = {name};
        integrityCheck(s, r, name == "quick_check");
        return r;
    }
    if (isStoragePragma(name)) {
        runStoragePragma(s, storeIdx, name, value, r);
        return r;
    }
    if (name == "compile_options") {
        r.columns = {"compile_options"};
        for (const char* o : {"DQS=0", "ENABLE_MATH_FUNCTIONS", "LIBSQL", "OMIT_LOAD_EXTENSION", "SOUNDEX",
                              "THREADSAFE=0"})
            r.rows.push_back({Value(o)});
        return r;
    }
    if (name == "collation_list") {
        r.columns = pragmaColumns(name);
        const char* names[] = {"RTRIM", "NOCASE", "BINARY"};
        for (std::int64_t i = 0; i < 3; ++i)
            r.rows.push_back({Value(i), Value(names[i])});
        return r;
    }
    if (name == "pragma_list") {
        r.columns = pragmaColumns(name);
        for (const char* p : kPragmas)
            r.rows.push_back({Value(p)});
        for (const char* p : storagePragmaNames())
            r.rows.push_back({Value(p)});
        return r;
    }
    if (name == "module_list") {
        r.columns = pragmaColumns(name);
        for (const TableFunctionDef& t : FunctionRegistry::builtins().tables())
            r.rows.push_back({Value(t.name)});
        return r;
    }
    if (name == "function_list") {
        r.columns = pragmaColumns(name);
        for (const FunctionDef& f : FunctionRegistry::builtins().all())
            r.rows.push_back({Value(f.name), Value(std::int64_t(1)), Value(f.aggregate ? "a" : "s"),
                              Value("utf8"), Value(std::int64_t(f.maxArgs < 0 ? -1 : f.maxArgs)),
                              Value(std::int64_t((f.flags & kDeterministic) ? 0x800 : 0))});
        return r;
    }
    if (name == "database_list") {
        r.columns = pragmaColumns(name);
        for (std::size_t i = 0; i < s.stores.size(); ++i) {
            if (!s.stores[i].catalog && i == 1)
                continue;
            r.rows.push_back({Value(std::int64_t(i)), Value(s.stores[i].name), Value(s.stores[i].path)});
        }
        return r;
    }
    if (name == "table_list") {
        r.columns = pragmaColumns(name);
        for (std::size_t i = 0; i < s.stores.size(); ++i) {
            if (!s.stores[i].catalog) {
                if (i == 1)
                    r.rows.push_back({Value("temp"), Value("sqlite_temp_schema"), Value("table"),
                                      Value(std::int64_t(5)), Value(std::int64_t(0)), Value(std::int64_t(0))});
                continue;
            }
            if (!schema.empty() && i != storeIdx)
                continue;
            const Schema& sc = s.catalog(i).schema();
            for (const auto& t : sc.tables) {
                if (!arg.empty() && !equalsNoCase(arg, t->name))
                    continue;
                r.rows.push_back({Value(s.stores[i].name), Value(t->name), Value("table"),
                                  Value(std::int64_t(t->columns.size())), Value(std::int64_t(t->withoutRowid)),
                                  Value(std::int64_t(t->strict))});
            }
            for (const auto& v : sc.views) {
                if (!arg.empty() && !equalsNoCase(arg, v->name))
                    continue;
                r.rows.push_back({Value(s.stores[i].name), Value(v->name), Value("view"),
                                  Value(std::int64_t(viewColumns(s, *v).size())), Value(std::int64_t(0)),
                                  Value(std::int64_t(0))});
            }
            if (arg.empty() || equalsNoCase(arg, i == 1 ? "sqlite_temp_schema" : "sqlite_schema"))
                r.rows.push_back({Value(s.stores[i].name), Value(i == 1 ? "sqlite_temp_schema" : "sqlite_schema"),
                                  Value("table"), Value(std::int64_t(5)), Value(std::int64_t(0)),
                                  Value(std::int64_t(0))});
        }
        return r;
    }
    if (name == "foreign_key_check") {
        r.columns = pragmaColumns(name);
        foreignKeyCheck(s, arg, r);
        return r;
    }
    if (name == "table_info" || name == "table_xinfo") {
        r.columns = pragmaColumns(name);
        if (!value)
            return r;
        std::size_t store = 0;
        const Table* t = pragmaTable(s, schema, arg, store);
        if (!t) {
            for (std::size_t i = 0; i < s.stores.size(); ++i) {
                if (!s.stores[i].catalog)
                    continue;
                if (const View* v = s.catalog(i).findView(arg)) {
                    std::int64_t cid = 0;
                    for (const std::string& n : viewColumns(s, *v)) {
                        std::vector<Value> row{Value(cid++), Value(n), Value(""), Value(std::int64_t(0)), Value(),
                                               Value(std::int64_t(0))};
                        if (name == "table_xinfo")
                            row.push_back(Value(std::int64_t(0)));
                        r.rows.push_back(std::move(row));
                    }
                    return r;
                }
            }
            return r;
        }
        std::int64_t cid = 0;
        for (std::size_t i = 0; i < t->columns.size(); ++i) {
            const Column& c = t->columns[i];
            if (c.generated && name == "table_info")
                continue;
            std::int64_t pk = 0;
            for (std::size_t k = 0; k < t->primaryKey.size(); ++k) {
                if (t->primaryKey[k] == int(i))
                    pk = std::int64_t(k + 1);
            }
            Value dflt;
            if (c.hasDefault)
                dflt = Value(c.defaultSql);
            std::vector<Value> row{Value(cid++), Value(c.name), Value(c.declType),
                                   Value(std::int64_t(c.declaredNotNull)),
                                   dflt, Value(pk)};
            if (name == "table_xinfo")
                row.push_back(Value(std::int64_t(c.generated == 1 ? 2 : (c.generated == 2 ? 3 : 0))));
            r.rows.push_back(std::move(row));
        }
        return r;
    }
    if (name == "index_list") {
        r.columns = pragmaColumns(name);
        if (!value)
            return r;
        std::size_t store = 0;
        const Table* t = pragmaTable(s, schema, arg, store);
        if (!t)
            return r;
        std::vector<const Index*> list(t->indexes.begin(), t->indexes.end());
        std::reverse(list.begin(), list.end());
        std::int64_t seq = 0;
        for (const Index* ix : list) {
            r.rows.push_back({Value(seq++), Value(ix->name), Value(std::int64_t(ix->unique)),
                              Value(ix->origin == 0 ? "c" : (ix->origin == 1 ? "u" : "pk")),
                              Value(std::int64_t(!ix->whereSql.empty()))});
        }
        return r;
    }
    if (name == "index_info" || name == "index_xinfo") {
        r.columns = pragmaColumns(name);
        if (!value)
            return r;
        for (std::size_t i = 0; i < s.stores.size(); ++i) {
            if (!s.stores[i].catalog)
                continue;
            if (!schema.empty() && i != storeIdx)
                continue;
            const Index* ix = s.catalog(i).findIndex(arg);
            if (!ix)
                continue;
            const Table* t = s.catalog(i).findTable(ix->table);
            std::int64_t seq = 0;
            for (const IndexPart& p : ix->parts) {
                const std::int64_t cid = p.column == -2 ? -1 : (p.column == -1 ? -2 : p.column);
                Value nm = p.column >= 0 ? Value(t->columns[std::size_t(p.column)].name) : Value();
                std::vector<Value> row{Value(seq++), Value(cid), nm};
                if (name == "index_xinfo") {
                    row.push_back(Value(std::int64_t(p.desc)));
                    row.push_back(Value(toString(p.collation)));
                    row.push_back(Value(std::int64_t(1)));
                }
                r.rows.push_back(std::move(row));
            }
            if (name == "index_xinfo") {
                if (t->hasRowid()) {
                    r.rows.push_back({Value(seq++), Value(std::int64_t(-1)), Value(), Value(std::int64_t(0)),
                                      Value("BINARY"), Value(std::int64_t(0))});
                } else {
                    for (const int pk : t->primaryKey)
                        r.rows.push_back({Value(seq++), Value(std::int64_t(pk)),
                                          Value(t->columns[std::size_t(pk)].name), Value(std::int64_t(0)),
                                          Value("BINARY"), Value(std::int64_t(0))});
                }
            }
            return r;
        }
        return r;
    }
    if (name == "foreign_key_list") {
        r.columns = pragmaColumns(name);
        if (!value)
            return r;
        std::size_t store = 0;
        const Table* t = pragmaTable(s, schema, arg, store);
        if (!t)
            return r;
        std::int64_t id = 0;
        for (std::size_t f = t->foreignKeys.size(); f-- > 0;) {
            const ForeignKey& fk = t->foreignKeys[f];
            for (std::size_t k = 0; k < fk.columns.size(); ++k) {
                Value to = k < fk.parentColumns.size() ? Value(fk.parentColumns[k]) : Value();
                r.rows.push_back({Value(id), Value(std::int64_t(k)), Value(fk.parentTable),
                                  Value(t->columns[std::size_t(fk.columns[k])].name), to,
                                  Value(actionName(fk.onUpdate)), Value(actionName(fk.onDelete)), Value("NONE")});
            }
            ++id;
        }
        return r;
    }
    // Unknown pragmas are ignored, as SQLite ignores them.
    return r;
}

// --------------------------------------------------------- schema tables -

void schemaRows(Session& s, std::size_t i, RowStore& out)
{
    if (!s.stores[i].catalog)
        return;
    const Schema& sc = s.catalog(i).schema();
    // In creation order (ids), as SQLite lists them.
    struct Item
    {
        std::uint64_t id;
        const char* type;
        std::string name, table, sql;
    };
    std::vector<Item> items;
    for (const auto& t : sc.tables)
        items.push_back({t->id, "table", t->name, t->name, t->sql});
    for (const auto& ix : sc.indexes)
        items.push_back({ix->id, "index", ix->name, ix->table, ix->sql});
    for (const auto& v : sc.views)
        items.push_back({v->id, "view", v->name, v->name, v->sql});
    for (const auto& t : sc.triggers)
        items.push_back({t->id, "trigger", t->name, t->table, t->sql});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.id < b.id; });
    for (const Item& it : items) {
        Value* row = out.append();
        row[0] = Value(it.type);
        row[1] = Value(it.name);
        row[2] = Value(it.table);
        row[3] = Value(std::int64_t(std::string(it.type) == "table" || std::string(it.type) == "index" ? it.id : 0));
        row[4] = it.sql.empty() ? Value() : Value(it.sql);
    }
}

namespace {

std::unique_ptr<SelectPlan> shapeOf(const std::vector<std::string>& names)
{
    auto sp = std::make_unique<SelectPlan>();
    sp->columns = names.size();
    sp->names = names;
    sp->subNames = names;
    sp->qualified.assign(names.size(), std::string());
    sp->affinity.assign(names.size(), Affinity::Blob);
    sp->hasAffinity.assign(names.size(), false);
    sp->collation.assign(names.size(), Collation::Binary);
    sp->collationStrength.assign(names.size(), 0);
    sp->declTypes.assign(names.size(), std::string());
    return sp;
}

}  // namespace

SubqueryPlan* Planner::planSchemaTable(std::string_view schema, std::string_view name)
{
    int store = -1;
    const bool master = equalsNoCase(name, "sqlite_schema") || equalsNoCase(name, "sqlite_master");
    const bool tempMaster = equalsNoCase(name, "sqlite_temp_schema") || equalsNoCase(name, "sqlite_temp_master");
    if (master) {
        store = schema.empty() ? 0 : session_.storeIndex(schema);
    } else if (tempMaster) {
        store = 1;
    } else if (equalsNoCase(name, "sqlite_sequence")) {
        SubqueryPlan* sub = newSubquery(SubqueryPlan::Kind::Rows);
        sub->select = shapeOf({"name", "seq"});
        sub->width = 2;
        const int si = schema.empty() ? 0 : session_.storeIndex(schema);
        if (si < 0)
            return nullptr;
        bool any = false;
        if (session_.stores[std::size_t(si)].catalog) {
            for (const auto& t : session_.catalog(std::size_t(si)).schema().tables)
                any = any || t->autoincrement;
        }
        if (!any)
            return nullptr;
        sub->generator = [this, si](Ctx& c, RowStore& out) {
            Session& s = *c.session;
            if (!s.stores[std::size_t(si)].catalog)
                return;
            Catalog& cat = s.catalog(std::size_t(si));
            for (const auto& t : cat.schema().tables) {
                if (!t->autoincrement)
                    continue;
                const std::int64_t seq = cat.sequence(*t);
                if (seq == 0)
                    continue;
                Value* row = out.append();
                row[0] = Value(t->name);
                row[1] = Value(seq);
            }
        };
        return sub;
    }
    if (store < 0)
        return nullptr;
    SubqueryPlan* sub = newSubquery(SubqueryPlan::Kind::Rows);
    sub->select = shapeOf({"type", "name", "tbl_name", "rootpage", "sql"});
    sub->width = 5;
    const std::size_t si = std::size_t(store);
    sub->generator = [si](Ctx& c, RowStore& out) { schemaRows(*c.session, si, out); };
    return sub;
}

SubqueryPlan* Planner::planPragmaFunction(std::string_view pragma, std::vector<ExprPtr>& args, Scope& scope)
{
    const std::string name = toLower(pragma);
    bool known = isStoragePragma(name);
    for (const char* p : kPragmas)
        known = known || name == p;
    if (!known)
        throw Error(ErrorCode::NoSuchTable, "no such table: pragma_" + name);
    std::vector<std::string> cols = pragmaColumns(name);
    cols.push_back("arg");
    cols.push_back("schema");
    SubqueryPlan* sub = newSubquery(SubqueryPlan::Kind::Rows);
    sub->select = shapeOf(cols);
    sub->width = cols.size();
    sub->correlated = true;
    std::vector<Expr*> argExprs;
    for (ExprPtr& a : args) {
        resolve(a, scope, 0);
        argExprs.push_back(a.get());
    }
    if (argExprs.size() > 2)
        throw Error(ErrorCode::Runtime, "too many arguments on pragma_" + name + "() - max 2");
    const std::size_t width = cols.size();
    sub->generator = [name, argExprs, width](Ctx& c, RowStore& out) {
        Value arg, schema;
        if (!argExprs.empty())
            arg = eval(*argExprs[0], c);
        if (argExprs.size() > 1)
            schema = eval(*argExprs[1], c);
        const std::string sch = schema.isNull() ? std::string() : text(schema);
        PragmaResult r = runPragma(*c.session, sch, name, argExprs.empty() ? nullptr : &arg, true);
        for (auto& row : r.rows) {
            Value* o = out.append();
            for (std::size_t i = 0; i < width - 2 && i < row.size(); ++i)
                o[i] = row[i];
            o[width - 2] = arg;
            o[width - 1] = schema;
        }
    };
    return sub;
}

}  // namespace sql::internal
