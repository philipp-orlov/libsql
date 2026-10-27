// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// PRAGMA storage_*: the store underneath, as rows. They are what the sql tool's Visualize tab reads, over the same
// SQL socket as everything else, and they are also pragma_storage_*() table-valued functions, so a script can join
// them: `SELECT * FROM pragma_storage_trees() ORDER BY leaf_pages DESC`. They read the committed state of the store
// (a snapshot of its own), never what a transaction holds pending, and write nothing.
//
//   storage_stats          name, value        the store's counters (nosql::EnvStats)
//   storage_meta           one row per meta page: ids, checksums, the transaction it commits
//   storage_trees          one row per tree: the SQL table or index it holds, root, depth, page counts
//   storage_map            runs of consecutive pages of one kind and owner
//   storage_free           free-list entries: the transaction that retired the pages, how many
//   storage_page(n)        the header of page n: kind, slots, free space, shared prefix, stored and computed hash
//   storage_nodes(n)       the keys and values (or child pages) on page n
//   storage_bytes(n)       the page's raw bytes (an overflow run: up to its first 16 pages)
//   storage_check          (result, page, detail): every page's hash, then the structural check

#include <cstdio>

#include "internal/stmt.hpp"
#include "internal/util.hpp"
#include "nosql/inspect.hpp"

namespace sql::internal {

namespace {

namespace ins = nosql::inspect;

struct StorageSpec
{
    const char* name;
    std::vector<std::string> columns;
};

const std::vector<StorageSpec>& specs()
{
    static const std::vector<StorageSpec> all = {
        {"storage_stats", {"name", "value"}},
        {"storage_meta",
         {"slot", "valid", "current", "version", "page_size", "txnid", "last_pgno", "file_size", "named_dbs", "checksum",
          "computed_checksum", "store_id", "commit_id", "parent_id", "parent_checksum", "deferred_count", "inline_count",
          "inline_txn"}},
        {"storage_trees",
         {"id", "name", "role", "kind", "object", "parent", "root", "depth", "branch_pages", "leaf_pages", "overflow_pages",
          "entries", "flags"}},
        {"storage_map", {"first", "count", "kind", "tree"}},
        {"storage_free", {"txn", "chunk", "pages", "first_pages", "deferred"}},
        {"storage_page",
         {"pgno", "kind", "flags", "prefix_keys", "suffix_keys", "page_size", "nkeys", "lower", "upper", "shared_bytes",
          "shared_block", "free_bytes", "used_bytes", "entries", "run_pages", "run_bytes", "stored_hash", "computed_hash",
          "hash_ok", "header_ok"}},
        {"storage_nodes",
         {"idx", "offset", "size", "key", "key_bytes", "value_bytes", "value", "big", "subdb", "child", "count", "overflow"}},
        {"storage_bytes", {"data", "pages"}},
        {"storage_check", {"result", "page", "detail", "pages_checked"}},
    };
    return all;
}

Value integer(std::uint64_t v)
{
    return Value(static_cast<std::int64_t>(v));
}

/// A 64-bit hash or checksum as 16 hex digits: an SQL INTEGER is signed and loses the top bit's meaning.
Value hex64(std::uint64_t v)
{
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(v));
    return Value(std::string_view(buffer, 16));
}

Value flag(bool b)
{
    return Value(std::int64_t(b ? 1 : 0));
}

std::uint64_t pageArgument(const Value* value, const std::string& pragma)
{
    if (!value || value->isNull())
        throw Error(ErrorCode::Runtime, "PRAGMA " + pragma + " needs a page number: " + pragma + "(5)");
    const std::int64_t n = value->toInteger();
    if (n < 0)
        throw Error(ErrorCode::Runtime, "a page number is not negative");
    return static_cast<std::uint64_t>(n);
}

}  // namespace

bool isStoragePragma(std::string_view name)
{
    for (const StorageSpec& s : specs())
        if (name == s.name)
            return true;
    return false;
}

std::vector<std::string> storagePragmaColumns(std::string_view name)
{
    for (const StorageSpec& s : specs())
        if (name == s.name)
            return s.columns;
    return {};
}

std::vector<const char*> storagePragmaNames()
{
    std::vector<const char*> out;
    for (const StorageSpec& s : specs())
        out.push_back(s.name);
    return out;
}

static void runStorageRows(Session& s, std::size_t storeIdx, const std::string& name, const Value* value, PragmaResult& r);

void runStoragePragma(Session& s, std::size_t storeIdx, const std::string& name, const Value* value, PragmaResult& r)
{
    try {
        runStorageRows(s, storeIdx, name, value, r);
    } catch (const nosql::Error& e) {
        throw Error(ErrorCode::Runtime, e.what());
    }
}

static void runStorageRows(Session& s, std::size_t storeIdx, const std::string& name, const Value* value, PragmaResult& r)
{
    r.columns = storagePragmaColumns(name);
    nosql::Env* env = s.stores[storeIdx].env;
    if (!env)
        return;  // a store with no file of its own
    nosql::Txn t = env->readTxn();
    auto row = [&](std::initializer_list<Value> cells) { r.rows.emplace_back(cells); };

    if (name == "storage_stats") {
        const nosql::EnvStats st = env->stats();
        const std::pair<const char*, std::uint64_t> counters[] = {
            {"page_size", st.pageSize},
            {"file_size", st.fileSize},
            {"map_size", st.mapSize},
            {"used_pages", st.usedPages},
            {"free_pages", st.freePages},
            {"last_txn", st.lastTxn},
            {"durable_txn", st.durableTxn},
            {"oldest_reader", st.oldestReader},
            {"readers", st.readers},
            {"buffer_bytes", st.bufferBytes},
            {"dirty_bytes", st.dirtyBytes},
            {"spilled_pages", st.spilledPages},
            {"unspilled_pages", st.unspilledPages},
            {"optimistic_commits", st.optimisticCommits},
            {"optimistic_queued", st.optimisticQueued},
            {"capture_failures", st.captureFailures},
            {"captured_txn", st.capturedTxnid},
        };
        for (const auto& [key, number] : counters)
            row({Value(key), integer(number)});
        return;
    }
    if (name == "storage_meta") {
        for (const ins::MetaInfo& m : ins::metas(t))
            row({integer(m.slot), flag(m.valid), flag(m.current), integer(m.version), integer(m.pageSize), integer(m.txnid),
                 integer(m.lastPgno), integer(m.fileSize), integer(m.namedDbs), hex64(m.checksum), hex64(m.computedChecksum),
                 Value(m.storeId), Value(m.commitId), Value(m.parentId), hex64(m.parentChecksum), integer(m.deferredCount),
                 integer(m.inlineCount), integer(m.inlineTxn)});
        return;
    }
    if (name == "storage_trees") {
        // Which SQL object each sub-database holds.
        struct Owner
        {
            std::string kind, object, parent;
        };
        std::vector<std::pair<std::string, Owner>> owners;
        const Schema& schema = s.catalog(storeIdx).schema();
        for (const auto& tb : schema.tables)
            owners.push_back({tb->dataDb, {"table", tb->name, ""}});
        for (const auto& ix : schema.indexes)
            owners.push_back({ix->entriesDb, {"index", ix->name, ix->table}});
        std::int64_t id = 0;
        for (const ins::TreeInfo& tr : ins::trees(t)) {
            Owner owner{"", "", ""};
            if (tr.role == ins::TreeRole::Named) {
                owner.kind = tr.name == "sql_catalog" ? "catalog" : "other";
                for (const auto& [db, o] : owners)
                    if (db == tr.name)
                        owner = o;
            } else {
                owner.kind = ins::toString(tr.role);
            }
            row({Value(id++), Value(tr.name), Value(ins::toString(tr.role)), Value(owner.kind),
                 owner.object.empty() ? Value() : Value(owner.object), owner.parent.empty() ? Value() : Value(owner.parent), tr.root == ~std::uint64_t(0) ? Value() : integer(tr.root), integer(tr.depth),
                 integer(tr.branchPages), integer(tr.leafPages), integer(tr.overflowPages), integer(tr.entries),
                 integer(tr.flags)});
        }
        return;
    }
    if (name == "storage_map") {
        for (const ins::MapRun& run : ins::map(t, ins::trees(t)))
            row({integer(run.first), integer(run.count), Value(ins::toString(run.kind)),
                 run.tree < 0 ? Value() : Value(std::int64_t(run.tree))});
        return;
    }
    if (name == "storage_free") {
        for (const ins::FreeEntry& e : ins::freeList(t)) {
            std::string pages;
            for (std::size_t i = 0; i < e.first.size(); ++i)
                pages += (i ? "," : "") + std::to_string(e.first[i]);
            row({integer(e.txn), integer(e.chunk), integer(e.pages), Value(pages), flag(e.deferred)});
        }
        return;
    }
    if (name == "storage_page") {
        const std::uint64_t pgno = pageArgument(value, name);
        const ins::PageInfo p = ins::page(t, pgno);
        row({integer(p.pgno), Value(ins::toString(p.kind)), integer(p.flags), flag(p.prefixKeys), flag(p.suffixKeys),
             integer(p.pageSize), integer(p.nkeys), integer(p.lower), integer(p.upper), integer(p.sharedBytes),
             integer(p.sharedBlock), integer(p.freeBytes), integer(p.usedBytes), integer(p.entries), integer(p.runPages),
             integer(p.runBytes), hex64(p.storedHash), hex64(p.computedHash), flag(p.hashOk), flag(p.headerOk)});
        return;
    }
    if (name == "storage_nodes") {
        for (const ins::NodeInfo& n : ins::nodes(t, pageArgument(value, name), 256))
            row({integer(n.index), integer(n.offset), integer(n.size), Value::blob(n.key.data(), n.key.size()),
                 integer(n.keyBytes), integer(n.valueBytes), Value::blob(n.value.data(), n.value.size()), flag(n.big),
                 flag(n.subdb), integer(n.child), integer(n.count), integer(n.overflow)});
        return;
    }
    if (name == "storage_bytes") {
        const std::uint64_t pgno = pageArgument(value, name);
        const ins::PageInfo p = ins::page(t, pgno);
        const unsigned shown = p.kind == ins::PageKind::Overflow ? std::min<unsigned>(p.runPages, 16) : 1;
        const std::string bytes = ins::pageBytes(t, pgno, shown);
        row({Value::blob(bytes.data(), bytes.size()), integer(p.kind == ins::PageKind::Overflow ? p.runPages : 1)});
        return;
    }
    if (name == "storage_check") {
        const ins::CheckResult c = ins::check(t);
        for (std::uint64_t pg : c.badPages)
            row({Value("page"), integer(pg), Value("the page's bytes do not match its hash, or nothing owns the page"),
                 integer(c.pagesChecked)});
        if (!c.integrity.empty())
            row({Value("integrity"), Value(), Value(c.integrity), integer(c.pagesChecked)});
        if (c.ok())
            row({Value("ok"), Value(), Value(""), integer(c.pagesChecked)});
        return;
    }
}

}  // namespace sql::internal
