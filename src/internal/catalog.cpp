// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Catalog persistence. Everything lives in one sub-database keyed by a one
// byte record kind, so listing tables is a prefix scan and nothing else has to
// know the layout. A version record changes with every DDL statement, which is
// what lets a transaction reuse the last decoded schema after one lookup.

#include "internal/catalog.hpp"

#include <algorithm>

#include "internal/encoding.hpp"
#include "internal/lexer.hpp"

namespace sql::internal {

namespace {

constexpr const char* kMetaDb = "sql_catalog";
constexpr char kTableRecord = 't';
constexpr char kIndexRecord = 'i';
constexpr char kRowidRecord = 'r';
constexpr char kVersionRecord = 'v';
/// Table records: version 1 carried one primary key position, version 2 a
/// list of them. Both are still read; only version 2 is written.
constexpr std::uint64_t kTableFormatV1 = 1;
constexpr std::uint64_t kTableFormat = 2;
constexpr std::uint64_t kIndexFormat = 1;

std::string metaKey(char kind, std::string_view name)
{
    std::string k(1, kind);
    k += toLower(name);
    return k;
}

std::string encodeTable(const Table& t)
{
    std::string out;
    putVarint(out, kTableFormat);
    putString(out, t.name);
    putVarint(out, t.columns.size());
    for (const ColumnInfo& c : t.columns) {
        putString(out, c.name);
        out.push_back(char(std::uint8_t(c.type)));
        out.push_back(char(std::uint8_t((c.notNull ? 1 : 0) | (c.unique ? 2 : 0) |
                                        (c.hasDefault ? 4 : 0))));
        if (c.hasDefault)
            putValue(out, c.defaultValue);
    }
    putVarint(out, t.primaryKey.size());
    for (const int c : t.primaryKey)
        putVarint(out, std::uint64_t(c));
    return out;
}

Table decodeTable(std::string_view in)
{
    const std::uint64_t version = getVarint(in);
    if (version != kTableFormatV1 && version != kTableFormat)
        throw Error(ErrorCode::Internal, "catalog written by a different libsql version");
    Table t;
    t.name = getString(in);
    const std::uint64_t count = getVarint(in);
    for (std::uint64_t i = 0; i < count; ++i) {
        ColumnInfo c;
        c.name = getString(in);
        if (in.size() < 2)
            throw Error(ErrorCode::Internal, "truncated catalog record");
        c.type = Type(static_cast<unsigned char>(in[0]));
        const auto flags = static_cast<unsigned char>(in[1]);
        in.remove_prefix(2);
        c.notNull = (flags & 1) != 0;
        c.unique = (flags & 2) != 0;
        c.hasDefault = (flags & 4) != 0;
        if (c.hasDefault)
            c.defaultValue = getValue(in);
        t.columns.push_back(std::move(c));
    }
    if (version == kTableFormatV1) {
        const int single = int(getVarint(in)) - 1;
        if (single >= 0)
            t.primaryKey.push_back(single);
    } else {
        const std::uint64_t keys = getVarint(in);
        for (std::uint64_t i = 0; i < keys; ++i)
            t.primaryKey.push_back(int(getVarint(in)));
    }
    for (const int c : t.primaryKey) {
        if (c < 0 || std::size_t(c) >= t.columns.size())
            throw Error(ErrorCode::Internal, "catalog record names a key column out of range");
        t.columns[std::size_t(c)].primaryKey = true;
    }
    return t;
}

std::string encodeIndex(const Index& ix)
{
    std::string out;
    putVarint(out, kIndexFormat);
    putString(out, ix.name);
    putString(out, ix.table);
    putVarint(out, ix.columns.size());
    for (const int c : ix.columns)
        putVarint(out, std::uint64_t(c));
    out.push_back(ix.unique ? 1 : 0);
    return out;
}

Index decodeIndex(std::string_view in)
{
    if (getVarint(in) != kIndexFormat)
        throw Error(ErrorCode::Internal, "catalog written by a different libsql version");
    Index ix;
    ix.name = getString(in);
    ix.table = getString(in);
    const std::uint64_t count = getVarint(in);
    for (std::uint64_t i = 0; i < count; ++i)
        ix.columns.push_back(int(getVarint(in)));
    if (in.empty())
        throw Error(ErrorCode::Internal, "truncated catalog record");
    ix.unique = in[0] != 0;
    return ix;
}

/// Case-insensitive ordering of table names, which is how Schema::tables is
/// kept so lookups can bisect.
bool lessNoCase(std::string_view a, std::string_view b) noexcept
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int x = std::tolower(static_cast<unsigned char>(a[i]));
        const int y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y)
            return x < y;
    }
    return a.size() < b.size();
}

/// Sorts the tables and renumbers every table and index.
void settle(Schema& s)
{
    std::sort(s.tables.begin(), s.tables.end(),
              [](const Table& a, const Table& b) { return lessNoCase(a.name, b.name); });
    unsigned nextIndex = 0;
    for (std::size_t i = 0; i < s.tables.size(); ++i) {
        Table& t = s.tables[i];
        t.settle(unsigned(i));
        for (Index& ix : t.indexes) {
            ix.entriesDb = "idx:" + toLower(ix.name);
            ix.id = nextIndex++;
        }
    }
    s.indexCount = nextIndex;
}

std::shared_ptr<const Schema> decodeSchema(nosql::Db& db, std::uint64_t version)
{
    auto s = std::make_shared<Schema>();
    s->version = version;
    for (auto [key, value] : db.prefix(nosql::Slice(&kTableRecord, 1))) {
        (void)key;
        s->tables.push_back(decodeTable(viewOf(value)));
    }
    for (auto [key, value] : db.prefix(nosql::Slice(&kIndexRecord, 1))) {
        (void)key;
        Index ix = decodeIndex(viewOf(value));
        Table* owner = nullptr;
        for (Table& t : s->tables) {
            if (equalsNoCase(t.name, ix.table)) {
                owner = &t;
                break;
            }
        }
        if (!owner)
            throw Error(ErrorCode::Internal, "index '" + ix.name + "' has no table");
        owner->indexes.push_back(std::move(ix));
    }
    settle(*s);
    return s;
}

std::uint64_t readVersion(nosql::Db& db)
{
    if (const auto raw = db.get(nosql::Slice(&kVersionRecord, 1))) {
        std::string_view in = viewOf(*raw);
        return getVarint(in);
    }
    return 0;
}

}  // namespace

// ------------------------------------------------------------------ table --

int Table::find(std::string_view column) const noexcept
{
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (equalsNoCase(columns[i].name, column))
            return int(i);
    }
    if (hasRowid() && equalsNoCase(column, "rowid"))
        return int(columns.size());
    return -1;
}

void Table::settle(unsigned tableId)
{
    id = tableId;
    dataDb = "tbl:" + toLower(name);
    rowidKey = metaKey(kRowidRecord, name);
    if (!primaryKey.empty())
        keySlots_ = primaryKey;
    else
        keySlots_.assign(1, int(columns.size()));
}

// ----------------------------------------------------------------- schema --

const Table* Schema::find(std::string_view name) const noexcept
{
    const auto it = std::lower_bound(tables.begin(), tables.end(), name,
                                     [](const Table& t, std::string_view n) {
                                         return lessNoCase(t.name, n);
                                     });
    if (it != tables.end() && equalsNoCase(it->name, name))
        return &*it;
    return nullptr;
}

const Index* Schema::findIndex(std::string_view name) const noexcept
{
    for (const Table& t : tables) {
        for (const Index& ix : t.indexes) {
            if (equalsNoCase(ix.name, name))
                return &ix;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- catalog --

Catalog::Catalog(nosql::Txn& txn, SchemaCache& cache) : txn_(&txn), cache_(&cache)
{
    // The committed snapshot this transaction reads: a writer builds on the
    // commit before the one it will become.
    const std::uint64_t snapshot = txn.isReadOnly() ? txn.id() : txn.id() - 1;
    bool seen;
    {
        std::lock_guard<std::mutex> lk(cache.mtx);
        seen = cache.catalogSeen;
        schema_ = cache.schema;
        if (schema_ && cache.validAtTxn == snapshot)
            return;
    }
    if (!seen) {
        if (!txn.hasDb(kMetaDb)) {
            // Nothing has ever been created: an empty schema, version zero.
            if (!schema_)
                schema_ = std::make_shared<Schema>();
            return;
        }
        std::lock_guard<std::mutex> lk(cache.mtx);
        cache.catalogSeen = true;
    }
    // One point lookup decides whether the cached snapshot is the one this
    // transaction sees; a snapshot older than the cache decodes privately
    // rather than replacing what newer transactions are using.
    meta_.db = txn.db(kMetaDb);
    meta_.open = true;
    const std::uint64_t version = readVersion(meta_.db);
    if (schema_ && schema_->version == version) {
        std::lock_guard<std::mutex> lk(cache.mtx);
        if (cache.schema == schema_ && cache.validAtTxn < snapshot)
            cache.validAtTxn = snapshot;
        return;
    }
    schema_ = decodeSchema(meta_.db, version);
    std::lock_guard<std::mutex> lk(cache.mtx);
    if (!cache.schema || cache.schema->version < version) {
        cache.schema = schema_;
        cache.validAtTxn = snapshot;
    }
}

const nosql::Db& Catalog::meta(bool create) const
{
    if (!meta_.open) {
        meta_.db = txn_->db(kMetaDb, create ? nosql::DbFlags::Create : nosql::DbFlags::None);
        meta_.open = true;
    }
    return meta_.db;
}

const Table& Catalog::get(std::string_view name) const
{
    if (const Table* t = find(name))
        return *t;
    throw Error(ErrorCode::NoSuchTable, "no such table: " + std::string(name));
}

std::vector<std::string> Catalog::tableNames() const
{
    std::vector<std::string> out;
    out.reserve(schema_->tables.size());
    for (const Table& t : schema_->tables)
        out.push_back(t.name);
    return out;
}

std::vector<const Index*> Catalog::allIndexes() const
{
    std::vector<const Index*> out;
    for (const Table& t : schema_->tables) {
        for (const Index& ix : t.indexes)
            out.push_back(&ix);
    }
    return out;
}

Schema& Catalog::edit()
{
    if (!edited_) {
        edited_ = std::make_shared<Schema>(*schema_);
        ++edited_->version;
        schema_ = edited_;
        storeVersion();
    }
    return *edited_;
}

void Catalog::storeVersion()
{
    std::string out;
    putVarint(out, edited_->version);
    meta(true).put(nosql::Slice(&kVersionRecord, 1), out);
}

void Catalog::resetHandles()
{
    tableHandles_.clear();
    indexHandles_.clear();
    rowids_.clear();
}

void Catalog::storeTable(const Table& table)
{
    meta(true).put(metaKey(kTableRecord, table.name), encodeTable(table));
}

void Catalog::addTable(Table table)
{
    flush();
    Schema& s = edit();
    storeTable(table);
    txn_->db("tbl:" + toLower(table.name), nosql::DbFlags::Create);
    s.tables.push_back(std::move(table));
    settle(s);
    resetHandles();
}

void Catalog::dropTable(const Table& table)
{
    flush();
    const std::string name = table.name;
    std::vector<std::string> indexNames;
    for (const Index& ix : table.indexes)
        indexNames.push_back(ix.name);
    Schema& s = edit();
    const nosql::Db& db = meta(true);
    for (const std::string& ix : indexNames) {
        db.erase(metaKey(kIndexRecord, ix));
        txn_->dropDb("idx:" + toLower(ix));
    }
    db.erase(metaKey(kTableRecord, name));
    db.erase(metaKey(kRowidRecord, name));
    txn_->dropDb("tbl:" + toLower(name));
    s.tables.erase(std::remove_if(s.tables.begin(), s.tables.end(),
                                  [&](const Table& t) { return equalsNoCase(t.name, name); }),
                   s.tables.end());
    settle(s);
    resetHandles();
}

void Catalog::addIndex(Index index)
{
    flush();
    Schema& s = edit();
    meta(true).put(metaKey(kIndexRecord, index.name), encodeIndex(index));
    txn_->db("idx:" + toLower(index.name), nosql::DbFlags::Create);
    for (Table& t : s.tables) {
        if (equalsNoCase(t.name, index.table)) {
            t.indexes.push_back(std::move(index));
            break;
        }
    }
    settle(s);
    resetHandles();
}

void Catalog::dropIndex(const Index& index)
{
    flush();
    const std::string table = index.table, name = index.name;
    Schema& s = edit();
    meta(true).erase(metaKey(kIndexRecord, name));
    txn_->dropDb("idx:" + toLower(name));
    for (Table& t : s.tables) {
        if (!equalsNoCase(t.name, table))
            continue;
        auto& list = t.indexes;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (equalsNoCase(it->name, name)) {
                list.erase(it);
                break;
            }
        }
    }
    settle(s);
    resetHandles();
}

const nosql::Db& Catalog::rows(const Table& table) const
{
    if (tableHandles_.size() < schema_->tables.size())
        tableHandles_.resize(schema_->tables.size());
    Handle& h = tableHandles_[table.id];
    if (!h.open) {
        h.db = txn_->db(table.dataDb);
        h.open = true;
    }
    return h.db;
}

const nosql::Db& Catalog::entries(const Index& index) const
{
    if (indexHandles_.size() < schema_->indexCount)
        indexHandles_.resize(schema_->indexCount);
    Handle& h = indexHandles_[index.id];
    if (!h.open) {
        h.db = txn_->db(index.entriesDb);
        h.open = true;
    }
    return h.db;
}

std::int64_t Catalog::lastRowid(const Table& table)
{
    if (rowids_.size() < schema_->tables.size())
        rowids_.resize(schema_->tables.size());
    Rowid& r = rowids_[table.id];
    if (!r.loaded) {
        r.last = 0;
        if (const auto raw = meta(true).get(table.rowidKey)) {
            std::string_view in = viewOf(*raw);
            r.last = std::int64_t(getVarint(in));
        }
        r.loaded = true;
    }
    return r.last;
}

std::int64_t Catalog::nextRowid(const Table& table)
{
    const std::int64_t next = lastRowid(table) + 1;
    Rowid& r = rowids_[table.id];
    r.last = next;
    r.dirty = true;
    return next;
}

void Catalog::noteRowid(const Table& table, std::int64_t used)
{
    if (used > 0 && used > lastRowid(table)) {
        Rowid& r = rowids_[table.id];
        r.last = used;
        r.dirty = true;
    }
}

void Catalog::flush()
{
    for (std::size_t i = 0; i < rowids_.size(); ++i) {
        Rowid& r = rowids_[i];
        if (!r.dirty)
            continue;
        std::string out;
        putVarint(out, std::uint64_t(r.last));
        meta(true).put(schema_->tables[i].rowidKey, out);
        r.dirty = false;
    }
}

void Catalog::committed(std::uint64_t txnid)
{
    std::lock_guard<std::mutex> lk(cache_->mtx);
    cache_->catalogSeen = cache_->catalogSeen || meta_.open;
    if (edited_) {
        if (!cache_->schema || cache_->schema->version < edited_->version) {
            cache_->schema = edited_;
            cache_->validAtTxn = txnid;
        }
        edited_.reset();
    } else if (cache_->schema == schema_ && cache_->validAtTxn < txnid) {
        cache_->validAtTxn = txnid;
    }
}

}  // namespace sql::internal
