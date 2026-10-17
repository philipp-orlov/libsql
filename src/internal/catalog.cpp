// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/catalog.hpp"

#include <algorithm>
#include <limits>

#include "internal/encoding.hpp"
#include "internal/parser.hpp"
#include "internal/util.hpp"

namespace sql::internal {

namespace {

constexpr const char* kMetaDb = "sql_catalog";
constexpr char kObject = 'o';
constexpr char kTrigger = 'g';
constexpr char kSequence = 'a';
constexpr char kPragma = 'p';
constexpr char kVersion = 'v';
constexpr char kNextId = 'n';
constexpr std::uint64_t kRecordFormat = 1;

std::string objectKey(char prefix, std::string_view name)
{
    std::string k(1, prefix);
    k += toLower(name);
    return k;
}

std::string idKey(char prefix, std::uint64_t id)
{
    std::string k(1, prefix);
    putVarint(k, id);
    return k;
}

std::string tableDb(std::uint64_t id)
{
    return "t#" + std::to_string(id);
}

std::string indexDb(std::uint64_t id)
{
    return "i#" + std::to_string(id);
}

struct Record
{
    char kind = 0;
    std::string name, table, sql;
    std::uint64_t id = 0;
};

Record decodeRecord(std::string_view in)
{
    if (getVarint(in) != kRecordFormat)
        throw Error(ErrorCode::Internal, "catalog written by a different libsql version");
    Record r;
    if (in.empty())
        throw Error(ErrorCode::Internal, "malformed catalog record");
    r.kind = in[0];
    in.remove_prefix(1);
    r.name = getString(in);
    r.table = getString(in);
    r.id = getVarint(in);
    r.sql = getString(in);
    return r;
}

std::int64_t readInt(const nosql::Db& db, std::string_view key, std::int64_t fallback)
{
    if (const auto raw = db.get(sliceOf(key))) {
        std::string_view in = viewOf(*raw);
        const std::uint64_t v = getVarint(in);
        return std::int64_t((v >> 1) ^ -(v & 1));
    }
    return fallback;
}

void writeInt(const nosql::Db& db, std::string_view key, std::int64_t v)
{
    std::string out;
    putVarint(out, (std::uint64_t(v) << 1) ^ std::uint64_t(v >> 63));
    db.put(sliceOf(key), sliceOf(out));
}

Trigger buildTrigger(const Record& r)
{
    const StatementNode node = parseOne(r.sql);
    const auto& s = std::get<CreateTriggerStmt>(node.body);
    Trigger t;
    t.name = r.name;
    t.table = s.table;
    t.sql = r.sql;
    t.time = s.time;
    t.event = s.event;
    t.updateOf = s.updateOf;
    t.id = r.id;
    return t;
}

}  // namespace

std::shared_ptr<const Schema> loadSchema(nosql::Txn& txn)
{
    auto s = std::make_shared<Schema>();
    if (!txn.hasDb(nosql::Slice(kMetaDb)))
        return s;
    nosql::Db db = txn.db(nosql::Slice(kMetaDb));
    // libsql 0.1 kept 't', 'i' and 'r' records here; this format never does.
    for (const char legacy : {'t', 'i', 'r'}) {
        for (auto entry : db.prefix(nosql::Slice(&legacy, 1))) {
            (void)entry;
            throw Error(ErrorCode::Internal,
                        "this store was written by libsql 0.1, whose catalog format is not supported");
        }
    }
    s->version = std::uint64_t(readInt(db, std::string_view(&kVersion, 1), 0));
    s->userVersion = readInt(db, std::string(1, kPragma) + "user_version", 0);
    s->applicationId = readInt(db, std::string(1, kPragma) + "application_id", 0);

    std::vector<Record> tables, indexes, views;
    for (auto [key, value] : db.prefix(nosql::Slice(&kObject, 1))) {
        (void)key;
        Record r = decodeRecord(viewOf(value));
        if (r.kind == 't')
            tables.push_back(std::move(r));
        else if (r.kind == 'i')
            indexes.push_back(std::move(r));
        else
            views.push_back(std::move(r));
    }
    std::vector<std::pair<std::string, StatementNode>> defs;
    for (Record& r : tables) {
        StatementNode node = parseOne(r.sql);
        auto t = std::make_unique<Table>(buildTable(std::get<CreateTableStmt>(node.body), r.sql));
        t->name = r.name;
        t->id = r.id;
        t->dataDb = tableDb(r.id);
        s->tables.push_back(std::move(t));
        defs.emplace_back(r.name, std::move(node));
    }
    for (Record& r : indexes) {
        const Table* t = s->findTable(r.table);
        if (!t)
            throw Error(ErrorCode::Internal, "index '" + r.name + "' has no table");
        Index ix;
        if (r.sql.empty()) {
            for (auto& d : defs) {
                if (!equalsNoCase(d.first, r.table))
                    continue;
                for (Index& cand : implicitIndexes(*t, std::get<CreateTableStmt>(d.second.body))) {
                    if (equalsNoCase(cand.name, r.name))
                        ix = std::move(cand);
                }
            }
            if (ix.name.empty())
                throw Error(ErrorCode::Internal, "implicit index '" + r.name + "' not derivable");
        } else {
            const StatementNode node = parseOne(r.sql);
            ix = buildIndex(std::get<CreateIndexStmt>(node.body), *t, r.sql);
        }
        ix.name = r.name;
        ix.id = r.id;
        ix.entriesDb = indexDb(r.id);
        s->indexes.push_back(std::make_unique<Index>(std::move(ix)));
    }
    for (Record& r : views) {
        auto v = std::make_unique<View>();
        v->name = r.name;
        v->sql = r.sql;
        v->id = r.id;
        s->views.push_back(std::move(v));
    }
    for (auto [key, value] : db.prefix(nosql::Slice(&kTrigger, 1))) {
        (void)key;
        s->triggers.push_back(std::make_unique<Trigger>(buildTrigger(decodeRecord(viewOf(value)))));
    }
    std::sort(s->triggers.begin(), s->triggers.end(),
              [](const auto& a, const auto& b) { return a->id < b->id; });
    std::sort(s->indexes.begin(), s->indexes.end(),
              [](const auto& a, const auto& b) { return a->id < b->id; });
    s->settle();
    return s;
}

void Catalog::close()
{
    levels_.clear();
    schema_.reset();
    edited_.reset();
    meta_ = Handle();
    for (Handle& h : tableHandles_)
        h = Handle();
    for (Handle& h : indexHandles_)
        h = Handle();
    rowids_.clear();
}

void Catalog::open(nosql::Txn& txn, SchemaCache& cache)
{
    close();
    cache_ = &cache;
    ++serial_;
    levels_.push_back(Level{&txn, nullptr, nullptr});
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
        if (!txn.hasDb(nosql::Slice(kMetaDb))) {
            if (!schema_ || schema_->version != 0)
                schema_ = std::make_shared<Schema>();
            return;
        }
        std::lock_guard<std::mutex> lk(cache.mtx);
        cache.catalogSeen = true;
    }
    const std::uint64_t version =
        std::uint64_t(readInt(meta(), std::string_view(&kVersion, 1), 0));
    if (schema_ && schema_->version == version) {
        std::lock_guard<std::mutex> lk(cache.mtx);
        if (cache.schema == schema_ && cache.validAtTxn < snapshot)
            cache.validAtTxn = snapshot;
        return;
    }
    schema_ = loadSchema(txn);
    std::lock_guard<std::mutex> lk(cache.mtx);
    if (!cache.schema || cache.schema->version < version) {
        cache.schema = schema_;
        cache.validAtTxn = snapshot;
    }
}

bool Catalog::readOnly() const
{
    return levels_.front().txn->isReadOnly();
}

const nosql::Db& Catalog::meta(bool create)
{
    if (meta_.serial != serial_) {
        meta_.db = txn().db(nosql::Slice(kMetaDb), create ? nosql::DbFlags::Create
                                                          : nosql::DbFlags::None);
        meta_.serial = serial_;
    } else if (create && !meta_.db.valid()) {
        meta_.db = txn().db(nosql::Slice(kMetaDb), nosql::DbFlags::Create);
    }
    return meta_.db;
}

Schema& Catalog::edit()
{
    if (!edited_) {
        edited_ = schema_->clone();
        ++edited_->version;
        schema_ = edited_;
        meta(true);
        storeVersion();
    }
    edited_->stamp = nextSchemaStamp();
    resetCaches();
    return *edited_;
}

void Catalog::storeVersion()
{
    writeInt(meta(true), std::string_view(&kVersion, 1), std::int64_t(edited_->version));
}

void Catalog::resetCaches()
{
    tableHandles_.clear();
    indexHandles_.clear();
    rowids_.clear();
}

std::uint64_t Catalog::newId()
{
    const nosql::Db& db = meta(true);
    const std::int64_t id = readInt(db, std::string_view(&kNextId, 1), 1);
    writeInt(db, std::string_view(&kNextId, 1), id + 1);
    return std::uint64_t(id);
}

void Catalog::putObject(char prefix, std::string_view name, char kind, std::string_view table,
                        std::uint64_t id, std::string_view sql)
{
    std::string out;
    putVarint(out, kRecordFormat);
    out.push_back(kind);
    putString(out, name);
    putString(out, table);
    putVarint(out, id);
    putString(out, sql);
    meta(true).put(sliceOf(objectKey(prefix, name)), sliceOf(out));
}

void Catalog::eraseObject(char prefix, std::string_view name)
{
    meta(true).erase(sliceOf(objectKey(prefix, name)));
}

void Catalog::addTable(Table t)
{
    Schema& s = edit();
    t.dataDb = tableDb(t.id);
    putObject(kObject, t.name, 't', t.name, t.id, t.sql);
    txn().db(sliceOf(t.dataDb), nosql::DbFlags::Create);
    s.tables.push_back(std::make_unique<Table>(std::move(t)));
    s.settle();
}

void Catalog::addIndex(Index ix)
{
    Schema& s = edit();
    ix.entriesDb = indexDb(ix.id);
    putObject(kObject, ix.name, 'i', ix.table, ix.id, ix.sql);
    txn().db(sliceOf(ix.entriesDb), nosql::DbFlags::Create);
    s.indexes.push_back(std::make_unique<Index>(std::move(ix)));
    s.settle();
}

void Catalog::addView(View v)
{
    Schema& s = edit();
    putObject(kObject, v.name, 'v', v.name, v.id, v.sql);
    s.views.push_back(std::make_unique<View>(std::move(v)));
}

void Catalog::addTrigger(Trigger t)
{
    Schema& s = edit();
    putObject(kTrigger, t.name, 'g', t.table, t.id, t.sql);
    s.triggers.push_back(std::make_unique<Trigger>(std::move(t)));
}

void Catalog::dropIndex(std::string_view name)
{
    Schema& s = edit();
    for (auto it = s.indexes.begin(); it != s.indexes.end(); ++it) {
        if (!equalsNoCase((*it)->name, name))
            continue;
        eraseObject(kObject, (*it)->name);
        if (txn().hasDb(sliceOf((*it)->entriesDb)))
            txn().dropDb(sliceOf((*it)->entriesDb));
        s.indexes.erase(it);
        break;
    }
    s.settle();
}

void Catalog::dropTable(std::string_view name)
{
    Schema& s = edit();
    std::vector<std::string> doomed;
    for (const auto& ix : s.indexes) {
        if (equalsNoCase(ix->table, name))
            doomed.push_back(ix->name);
    }
    for (const std::string& n : doomed)
        dropIndex(n);
    std::vector<std::string> triggers;
    for (const auto& tr : s.triggers) {
        if (equalsNoCase(tr->table, name))
            triggers.push_back(tr->name);
    }
    for (const std::string& n : triggers)
        dropTrigger(n);
    for (auto it = s.tables.begin(); it != s.tables.end(); ++it) {
        if (!equalsNoCase((*it)->name, name))
            continue;
        eraseObject(kObject, (*it)->name);
        meta(true).erase(sliceOf(idKey(kSequence, (*it)->id)));
        if (txn().hasDb(sliceOf((*it)->dataDb)))
            txn().dropDb(sliceOf((*it)->dataDb));
        s.tables.erase(it);
        break;
    }
    s.settle();
}

void Catalog::dropView(std::string_view name)
{
    Schema& s = edit();
    for (auto it = s.views.begin(); it != s.views.end(); ++it) {
        if (equalsNoCase((*it)->name, name)) {
            eraseObject(kObject, (*it)->name);
            s.views.erase(it);
            return;
        }
    }
}

void Catalog::dropTrigger(std::string_view name)
{
    Schema& s = edit();
    for (auto it = s.triggers.begin(); it != s.triggers.end(); ++it) {
        if (equalsNoCase((*it)->name, name)) {
            eraseObject(kTrigger, (*it)->name);
            s.triggers.erase(it);
            return;
        }
    }
}

void Catalog::replaceTable(std::string_view oldName, Table t)
{
    Schema& s = edit();
    for (auto& p : s.tables) {
        if (!equalsNoCase(p->name, oldName))
            continue;
        eraseObject(kObject, oldName);
        t.id = p->id;
        t.dataDb = p->dataDb;
        putObject(kObject, t.name, 't', t.name, t.id, t.sql);
        *p = std::move(t);
        break;
    }
    s.settle();
}

void Catalog::replaceIndex(std::string_view oldName, Index ix)
{
    Schema& s = edit();
    for (auto& p : s.indexes) {
        if (!equalsNoCase(p->name, oldName))
            continue;
        eraseObject(kObject, oldName);
        ix.id = p->id;
        ix.entriesDb = p->entriesDb;
        putObject(kObject, ix.name, 'i', ix.table, ix.id, ix.sql);
        *p = std::move(ix);
        break;
    }
    s.settle();
}

void Catalog::replaceView(std::string_view oldName, View v)
{
    Schema& s = edit();
    for (auto& p : s.views) {
        if (!equalsNoCase(p->name, oldName))
            continue;
        eraseObject(kObject, oldName);
        v.id = p->id;
        putObject(kObject, v.name, 'v', v.name, v.id, v.sql);
        *p = std::move(v);
        break;
    }
}

void Catalog::replaceTrigger(std::string_view oldName, Trigger t)
{
    Schema& s = edit();
    for (auto& p : s.triggers) {
        if (!equalsNoCase(p->name, oldName))
            continue;
        eraseObject(kTrigger, oldName);
        t.id = p->id;
        putObject(kTrigger, t.name, 'g', t.table, t.id, t.sql);
        *p = std::move(t);
        break;
    }
}

void Catalog::setUserVersion(std::int64_t v)
{
    Schema& s = edit();
    s.userVersion = v;
    writeInt(meta(true), std::string(1, kPragma) + "user_version", v);
}

void Catalog::setApplicationId(std::int64_t v)
{
    Schema& s = edit();
    s.applicationId = v;
    writeInt(meta(true), std::string(1, kPragma) + "application_id", v);
}

const nosql::Db& Catalog::rows(const Table& t)
{
    if (t.position >= schema_->tables.size() || schema_->tables[t.position].get() != &t)
        throw Error(ErrorCode::Internal, "table " + t.name + " is from a replaced schema");
    if (tableHandles_.size() < schema_->tables.size())
        tableHandles_.resize(schema_->tables.size());
    Handle& h = tableHandles_[t.position];
    if (h.serial != serial_) {
        h.db = txn().db(sliceOf(t.dataDb), readOnly() ? nosql::DbFlags::None : nosql::DbFlags::Create);
        h.serial = serial_;
    }
    return h.db;
}

const nosql::Db& Catalog::entries(const Index& ix)
{
    if (ix.position >= schema_->indexes.size() || schema_->indexes[ix.position].get() != &ix)
        throw Error(ErrorCode::Internal, "index " + ix.name + " is from a replaced schema");
    if (indexHandles_.size() < schema_->indexes.size())
        indexHandles_.resize(schema_->indexes.size());
    Handle& h = indexHandles_[ix.position];
    if (h.serial != serial_) {
        h.db = txn().db(sliceOf(ix.entriesDb),
                        readOnly() ? nosql::DbFlags::None : nosql::DbFlags::Create);
        h.serial = serial_;
    }
    return h.db;
}

Catalog::RowidState& Catalog::rowidState(const Table& t)
{
    if (t.position >= schema_->tables.size() || schema_->tables[t.position].get() != &t)
        throw Error(ErrorCode::Internal, "table " + t.name + " is from a replaced schema");
    if (rowids_.size() < schema_->tables.size())
        rowids_.resize(schema_->tables.size());
    return rowids_[t.position];
}

std::int64_t Catalog::largestRowid(const Table& t)
{
    // Only for tables keyed by their rowid: the largest one is the last key.
    RowidState& st = rowidState(t);
    if (!st.known) {
        st.max = 0;
        nosql::Cursor cur = rows(t).cursor();
        if (cur.last()) {
            std::string_view k = viewOf(cur.key());
            st.max = readRowidKey(k);
        }
        st.known = true;
    }
    return st.max;
}

std::int64_t Catalog::sequence(const Table& t)
{
    RowidState& st = rowidState(t);
    if (!st.seqLoaded) {
        st.seq = readInt(meta(), idKey(kSequence, t.id), 0);
        st.seqLoaded = true;
    }
    return st.seq;
}

void Catalog::setSequence(const Table& t, std::int64_t seq)
{
    RowidState& st = rowidState(t);
    st.seq = seq;
    st.seqLoaded = true;
    st.seqDirty = true;
}

std::int64_t Catalog::nextRowid(const Table& t)
{
    // Rowid-keyed tables follow SQLite: one past the largest rowid in use
    // (past the high-water mark too, under AUTOINCREMENT). A table clustered
    // by a declared key keeps its hidden rowids in the payload, so it keeps a
    // persisted high-water mark instead of searching for the largest.
    std::int64_t top = t.keyedByRowid() ? largestRowid(t) : sequence(t);
    if (t.autoincrement)
        top = std::max(top, sequence(t));
    if (top == std::numeric_limits<std::int64_t>::max())
        throw Error(ErrorCode::Runtime, "database or disk is full");
    return top < 0 ? 1 : top + 1;
}

void Catalog::noteRowid(const Table& t, std::int64_t rowid)
{
    if (t.keyedByRowid()) {
        RowidState& st = rowidState(t);
        if (st.known && rowid > st.max)
            st.max = rowid;
    }
    if ((t.autoincrement || t.payloadRowid()) && rowid > sequence(t))
        setSequence(t, rowid);
}

void Catalog::rowidRemoved(const Table& t, std::int64_t rowid)
{
    if (!t.keyedByRowid())
        return;
    RowidState& st = rowidState(t);
    if (st.known && rowid >= st.max)
        st.known = false;
}

void Catalog::flush()
{
    for (std::size_t i = 0; i < rowids_.size(); ++i) {
        RowidState& st = rowids_[i];
        if (!st.seqDirty)
            continue;
        writeInt(meta(true), idKey(kSequence, schema_->tables[i]->id), st.seq);
        st.seqDirty = false;
    }
}

void Catalog::committed(std::uint64_t txnid)
{
    std::lock_guard<std::mutex> lk(cache_->mtx);
    if (edited_) {
        cache_->schema = edited_;
        cache_->catalogSeen = true;
    }
    if (cache_->schema == schema_)
        cache_->validAtTxn = txnid;
}

void Catalog::pushLevel(nosql::Txn& child)
{
    levels_.push_back(Level{&child, schema_, edited_});
    ++serial_;
}

void Catalog::popLevel(bool committedChild)
{
    const Level top = levels_.back();
    levels_.pop_back();
    ++serial_;
    if (!committedChild) {
        if (schema_ != top.schema) {
            schema_ = top.schema;
            edited_ = top.edited;
        }
        resetCaches();
    }
}

}  // namespace sql::internal
