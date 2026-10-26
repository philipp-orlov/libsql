// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Prints tools/sql/ui/src/app/remote/golden.ts: requests and replies of the remote protocol, built with the
// same functions the C++ client and server use (src/remote/protocol.cpp), as base64 beside the fields they were
// built from. The TypeScript encoder must reproduce the request bytes exactly and the decoder must read the reply
// bytes back into the fields.

#include <cstdio>
#include <string>
#include <vector>

#include "bufs/bufs.hpp"
#include "remote/protocol.hpp"

using namespace sql;
namespace d = sql::remote::detail;
using sql::remote::Catalog;
using sql::remote::Kind;
using sql::remote::Op;

namespace {

std::string base64(const std::uint8_t* data, std::size_t size)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const std::size_t left = size - i;
        const unsigned v = (unsigned(data[i]) << 16) | (left > 1 ? unsigned(data[i + 1]) << 8 : 0) | (left > 2 ? data[i + 2] : 0);
        out += digits[(v >> 18) & 63];
        out += digits[(v >> 12) & 63];
        out += left > 1 ? digits[(v >> 6) & 63] : '=';
        out += left > 2 ? digits[v & 63] : '=';
    }
    return out;
}

std::string quote(std::string_view text)
{
    std::string out = "'";
    for (char c : text) {
        if (c == '\'' || c == '\\')
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out + "'";
}

/// The fields of a request that differ from the defaults, as a TypeScript object literal for RequestFields.
std::string requestJson(const d::RequestFields& f)
{
    std::string out = "{ id: " + std::to_string(f.id) + ", op: " + std::to_string(int(f.op));
    auto num = [&](const char* name, std::uint64_t v) {
        if (v)
            out += std::string(", ") + name + ": " + std::to_string(v);
    };
    auto str = [&](const char* name, std::string_view v) {
        if (!v.empty())
            out += std::string(", ") + name + ": " + quote(v);
    };
    str("sql", f.sql);
    num("statement", f.statement);
    num("txn", f.txn);
    num("target", f.target);
    num("credit", f.credit);
    if (f.readOnly)
        out += ", readOnly: true";
    num("what", std::uint64_t(f.what));
    str("name", f.name);
    num("protocol", f.protocol);
    str("client", f.client);
    str("token", f.token);
    num("window", f.window);
    num("maxMessage", f.maxMessage);
    num("hints", f.hints);
    auto value = [](const Value& v) -> std::string {
        switch (v.type()) {
            case Type::Integer: return "{ type: 1, value: " + std::to_string(v.integer()) + " }";
            case Type::Real: return "{ type: 2, value: " + std::to_string(v.real()) + " }";
            case Type::Datetime: return "{ type: 3, value: " + std::to_string(v.datetime().usec) + " }";
            case Type::Text: return "{ type: 4, value: " + quote(v.text()) + " }";
            case Type::Blob: {
                std::string bytes = "{ type: 5, value: new Uint8Array([";
                for (std::size_t i = 0; i < v.blob().size(); ++i)
                    bytes += (i ? "," : "") + std::to_string(int(std::to_integer<unsigned char>(v.blob()[i])));
                return bytes + "]) }";
            }
            default: return "null";
        }
    };
    if (f.named) {
        out += ", named: [";
        bool first = true;
        for (const auto& [name, v] : *f.named) {
            out += (first ? "" : ", ") + std::string("{ name: ") + quote(name) + ", value: " + value(v) + " }";
            first = false;
        }
        out += "]";
    } else if (f.paramCount) {
        out += ", params: [";
        for (std::size_t i = 0; i < f.paramCount; ++i)
            out += (i ? ", " : "") + value(f.params[i]);
        out += "]";
    }
    if (f.options) {
        out += ", options: { maxRows: " + std::to_string(f.maxRows) + ", batchRows: " + std::to_string(f.batchRows) +
               ", batchBytes: " + std::to_string(f.batchBytes) + ", blobPrefix: " + std::to_string(f.blobPrefix) +
               ", countAllRows: " + (f.countAllRows ? "true" : "false") + ", noDictionary: " + (f.noDictionary ? "true" : "false") +
               ", noNarrowing: " + (f.noNarrowing ? "true" : "false") + " }";
    }
    return out + " }";
}

std::string list(const std::vector<std::string>* v)
{
    std::string out = "[";
    if (v)
        for (std::size_t i = 0; i < v->size(); ++i)
            out += (i ? ", " : "") + quote((*v)[i]);
    return out + "]";
}

/// The Reply the TypeScript decoder should read back, as an object literal with every field.
std::string replyJson(const d::ReplyFields& f)
{
    return "{ id: " + std::to_string(f.id) + ", kind: " + std::to_string(int(f.kind)) + ", code: " + std::to_string(f.code) +
           ", constraint: " + std::to_string(f.constraint) + ", message: " + quote(f.message) +
           ", changes: " + std::to_string(f.changes) + ", lastInsertId: " + std::to_string(f.lastInsertId) +
           ", totalChanges: " + std::to_string(f.totalChanges) + ", inTransaction: " + (f.inTransaction ? "true" : "false") +
           ", elapsedUs: " + std::to_string(f.elapsedUs) + ", parameters: " + std::to_string(f.parameters) +
           ", parameterNames: " + list(f.parameterNames) + ", columns: " + list(f.columns) +
           ", writes: " + (f.writes ? "true" : "false") + ", protocol: " + std::to_string(f.protocol) +
           ", server: " + quote(f.server) + ", session: " + std::to_string(f.session) +
           ", readOnly: " + (f.readOnly ? "true" : "false") + ", window: " + std::to_string(f.window) +
           ", maxMessage: " + std::to_string(f.maxMessage) + ", maxPipeline: " + std::to_string(f.maxPipeline) +
           ", sql: " + quote(f.sql) + ", strict: " + (f.strict ? "true" : "false") +
           ", withoutRowid: " + (f.withoutRowid ? "true" : "false") + ", handleChanges: " + std::to_string(f.handleChanges) +
           ", handleLastInsertId: " + std::to_string(f.handleLastInsertId) + " }";
}

}  // namespace

int printRemoteGolden()
{
    std::printf("// Generated by tools/bufs_golden --remote. Do not edit.\n");
    std::printf("import type { RequestFields } from './request';\n\n");
    std::printf("export interface GoldenRequest {\n  name: string;\n  fields: RequestFields;\n  /** base64 of the bytes src/remote/protocol.cpp builds */\n  bytes: string;\n}\n\n");
    std::printf("export interface GoldenReply {\n  name: string;\n  /** every field of the decoded Reply except `size` */\n  fields: Record<string, unknown>;\n  bytes: string;\n}\n\n");

    std::vector<std::pair<std::string, d::RequestFields>> requests;
    auto add = [&](const char* name, d::RequestFields f) { requests.emplace_back(name, std::move(f)); };
    {
        d::RequestFields f;
        f.id = 1;
        f.op = Op::Hello;
        f.protocol = 1;
        f.client = "sql-ui/1.0";
        f.token = "s3cret token";
        f.readOnly = true;
        f.window = 1u << 20;
        f.maxMessage = 16u << 20;
        add("hello", f);
        f.token = {};
        f.readOnly = false;
        f.id = 2;
        add("hello_without_token", f);
    }
    static const std::vector<Value> mixed = {Value(7), Value(-9223372036854775807LL - 1), Value(2.5), Value("h\xc3\xa9llo \xe2\x82\xac"),
                                            Value::blob("\x00\x01\xff", 3), Value(), Value(Datetime{1700000000123456LL}), Value("")};
    {
        d::RequestFields f;
        f.id = 3;
        f.op = Op::Exec;
        f.sql = "SELECT * FROM t WHERE a = ?";
        f.hints = 1;
        add("exec_plain", f);
        f.id = 4;
        f.params = mixed.data();
        f.paramCount = mixed.size();
        add("exec_every_parameter_type", f);
        f.id = 5;
        f.options = true;
        f.batchBytes = 65536;
        f.blobPrefix = 32;
        f.maxRows = 5000;
        f.noDictionary = true;
        f.countAllRows = true;
        add("exec_with_options", f);
    }
    static const NamedParams named = {{":id", Value(42)}, {"name", Value("Ada")}, {":when", Value(Datetime{86400000000LL})}};
    {
        d::RequestFields f;
        f.id = 6;
        f.op = Op::Exec;
        f.sql = "SELECT :id, :name, :when";
        f.named = &named;
        f.txn = 3;
        add("exec_named_in_transaction", f);
    }
    {
        d::RequestFields f;
        f.id = 7;
        f.op = Op::Prepare;
        f.statement = 12;
        f.sql = "INSERT INTO t VALUES (?, ?)";
        add("prepare", f);
        f.id = 8;
        f.op = Op::ExecPrepared;
        f.sql = {};
        f.params = mixed.data();
        f.paramCount = 2;
        add("exec_prepared", f);
        d::RequestFields c;
        c.id = 9;
        c.op = Op::ClosePrepared;
        c.statement = 12;
        add("close_prepared", c);
    }
    {
        d::RequestFields f;
        f.id = 10;
        f.op = Op::Begin;
        f.txn = 5;
        f.readOnly = true;
        add("begin_read_only", f);
        d::RequestFields g;
        g.id = 11;
        g.op = Op::Commit;
        g.txn = 5;
        add("commit", g);
        d::RequestFields h;
        h.id = 12;
        h.op = Op::Interrupt;
        h.target = 4;
        add("interrupt", h);
        d::RequestFields c;
        c.id = 13;
        c.op = Op::Credit;
        c.credit = 524288;
        add("credit", c);
        d::RequestFields describe;
        describe.id = 14;
        describe.op = Op::Describe;
        describe.what = Catalog::Table_;
        describe.name = "customer";
        add("describe_table", describe);
        d::RequestFields ping;
        ping.id = 15;
        ping.op = Op::Ping;
        add("ping", ping);
        d::RequestFields bye;
        bye.id = 16;
        bye.op = Op::Goodbye;
        add("goodbye", bye);
    }
    std::printf("export const REQUESTS: GoldenRequest[] = [\n");
    for (auto& [name, f] : requests) {
        bufs::MessageBuilder mb;
        const auto [data, size] = d::buildRequest(mb, f);
        std::printf("  {\n    name: %s,\n    fields: %s,\n    bytes: %s,\n  },\n", quote(name).c_str(), requestJson(f).c_str(),
                    quote(base64(data, size)).c_str());
    }
    std::printf("];\n\n");

    static const std::vector<std::string> names = {":id", ":name"}, columns = {"id", "name", "price"};
    std::vector<std::pair<std::string, d::ReplyFields>> replies;
    {
        d::ReplyFields f;
        f.id = 1;
        f.kind = Kind::Welcome;
        f.protocol = 1;
        f.server = "sql/0.2.0";
        f.session = 77;
        f.readOnly = true;
        f.window = 1u << 20;
        f.maxMessage = 1u << 20;
        f.maxPipeline = 16;
        replies.emplace_back("welcome", f);
        d::ReplyFields rows;
        rows.id = 3;
        rows.kind = Kind::Rows;
        replies.emplace_back("rows", rows);
        d::ReplyFields done;
        done.id = 3;
        done.kind = Kind::Done;
        done.changes = 2;
        done.lastInsertId = 9007199254740991LL;
        done.totalChanges = 12;
        done.inTransaction = true;
        done.elapsedUs = 1234567;
        done.handleChanges = 2;
        done.handleLastInsertId = -5;
        replies.emplace_back("done", done);
        d::ReplyFields error;
        error.id = 4;
        error.kind = Kind::Error;
        error.code = 11;
        error.constraint = 1;
        error.message = "UNIQUE constraint failed: t.a \xe2\x82\xac";
        replies.emplace_back("error", error);
        d::ReplyFields prepared;
        prepared.id = 7;
        prepared.kind = Kind::Prepared;
        prepared.parameters = 2;
        prepared.parameterNames = &names;
        prepared.columns = &columns;
        prepared.writes = true;
        replies.emplace_back("prepared", prepared);
        d::ReplyFields table;
        table.id = 14;
        table.kind = Kind::Done;
        table.sql = "CREATE TABLE customer(id INTEGER PRIMARY KEY, name TEXT)";
        table.strict = true;
        table.withoutRowid = true;
        replies.emplace_back("done_with_table", table);
        d::ReplyFields pong;
        pong.id = 15;
        pong.kind = Kind::Pong;
        replies.emplace_back("pong", pong);
        d::ReplyFields closing;
        closing.kind = Kind::Closing;
        replies.emplace_back("closing", closing);
    }
    std::printf("export const REPLIES: GoldenReply[] = [\n");
    for (auto& [name, f] : replies) {
        const std::size_t bound = d::replyBound(f);
        std::vector<std::uint64_t> words((bound + 7) / 8);
        const std::size_t size = d::buildReply(words.data(), bound, f);
        std::printf("  {\n    name: %s,\n    fields: %s,\n    bytes: %s,\n  },\n", quote(name).c_str(), replyJson(f).c_str(),
                    quote(base64(reinterpret_cast<const std::uint8_t*>(words.data()), size)).c_str());
    }
    std::printf("];\n");
    return 0;
}
