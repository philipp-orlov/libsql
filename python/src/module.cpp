// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql for Python: the SQL engine and the side-car blob archive, bound with
// pybind11. See python/README.md for the Python-side view.
//
// Design notes
//
//   * Blobs never cross the boundary by copy. A Blob exports the buffer
//     protocol over the archive mapping it pins, so `memoryview`,
//     `numpy.frombuffer` and `torch.frombuffer` see the bytes where they lie.
//   * Everything that touches the store or the archive releases the GIL, so
//     a loader thread pool overlaps I/O with the Python side.
//   * Keys may be bytes, str or int. An int becomes 8 big-endian bytes so
//     that byte order and numeric order agree, which is what makes a sorted
//     batch probe and a prefix scan work over integer ids.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <mutex>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nosql/blob_storage.hpp"
#include "sql/sql.hpp"

namespace py = pybind11;

namespace {

// ------------------------------------------------------------- exceptions --

py::handle gError;       // libsql.Error, the base
py::handle gSqlError;    // libsql.SqlError:   sql::Error, .code = "NoSuchTable" ...
py::handle gStoreError;  // libsql.StoreError: nosql::Error, .code = "Busy" ...

const char* codeName(sql::ErrorCode c)
{
    switch (c) {
    case sql::ErrorCode::Ok: return "Ok";
    case sql::ErrorCode::SyntaxError: return "SyntaxError";
    case sql::ErrorCode::Unsupported: return "Unsupported";
    case sql::ErrorCode::InvalidArgument: return "InvalidArgument";
    case sql::ErrorCode::NoSuchTable: return "NoSuchTable";
    case sql::ErrorCode::TableExists: return "TableExists";
    case sql::ErrorCode::NoSuchColumn: return "NoSuchColumn";
    case sql::ErrorCode::AmbiguousColumn: return "AmbiguousColumn";
    case sql::ErrorCode::NoSuchIndex: return "NoSuchIndex";
    case sql::ErrorCode::IndexExists: return "IndexExists";
    case sql::ErrorCode::TypeMismatch: return "TypeMismatch";
    case sql::ErrorCode::ConstraintViolation: return "ConstraintViolation";
    case sql::ErrorCode::BadTransaction: return "BadTransaction";
    case sql::ErrorCode::ReadOnly: return "ReadOnly";
    case sql::ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

const char* codeName(nosql::ErrorCode c)
{
    switch (c) {
    case nosql::ErrorCode::Ok: return "Ok";
    case nosql::ErrorCode::NotFound: return "NotFound";
    case nosql::ErrorCode::KeyExists: return "KeyExists";
    case nosql::ErrorCode::Corrupted: return "Corrupted";
    case nosql::ErrorCode::InvalidArgument: return "InvalidArgument";
    case nosql::ErrorCode::Unsupported: return "Unsupported";
    case nosql::ErrorCode::KeyTooLarge: return "KeyTooLarge";
    case nosql::ErrorCode::ValueTooLarge: return "ValueTooLarge";
    case nosql::ErrorCode::MapFull: return "MapFull";
    case nosql::ErrorCode::TooManyDbs: return "TooManyDbs";
    case nosql::ErrorCode::Incompatible: return "Incompatible";
    case nosql::ErrorCode::BadTransaction: return "BadTransaction";
    case nosql::ErrorCode::ReadOnly: return "ReadOnly";
    case nosql::ErrorCode::Busy: return "Busy";
    case nosql::ErrorCode::IoError: return "IoError";
    case nosql::ErrorCode::OutOfMemory: return "OutOfMemory";
    }
    return "Unknown";
}

void raise(py::handle type, const char* what, const char* code)
{
    py::object inst = py::reinterpret_borrow<py::object>(type)(what);
    inst.attr("code") = code;
    PyErr_SetObject(type.ptr(), inst.ptr());
}

// ---------------------------------------------------------------- values ---

/// datetime.datetime(1970, 1, 1, tzinfo=utc), kept for the process lifetime.
py::handle epoch()
{
    static py::handle e = [] {
        py::module_ dt = py::module_::import("datetime");
        return dt.attr("datetime")(1970, 1, 1, 0, 0, 0, 0, dt.attr("timezone").attr("utc")).release();
    }();
    return e;
}

py::object datetimeFromUsec(std::int64_t usec)
{
    py::module_ dt = py::module_::import("datetime");
    return py::reinterpret_borrow<py::object>(epoch()) + dt.attr("timedelta")(0, 0, usec);
}

std::int64_t usecFromDatetime(py::handle o)
{
    py::module_ dt = py::module_::import("datetime");
    py::object d = py::reinterpret_borrow<py::object>(o);
    if (!py::isinstance(d, dt.attr("datetime")))
        d = dt.attr("datetime")(d.attr("year"), d.attr("month"), d.attr("day"));  // a date
    if (d.attr("tzinfo").is_none())
        d = d.attr("replace")(py::arg("tzinfo") = dt.attr("timezone").attr("utc"));
    py::object delta = d - py::reinterpret_borrow<py::object>(epoch());
    // Exact integer arithmetic: total_seconds() would round at microseconds.
    return delta.attr("days").cast<std::int64_t>() * 86400000000LL +
           delta.attr("seconds").cast<std::int64_t>() * 1000000LL +
           delta.attr("microseconds").cast<std::int64_t>();
}

bool isDatetimeLike(py::handle o)
{
    py::module_ dt = py::module_::import("datetime");
    return py::isinstance(o, dt.attr("date"));  // datetime is a date subclass
}

/// A C-contiguous view over anything exporting the buffer protocol.
struct Bytes
{
    py::buffer_info info;
    const void* data = nullptr;
    std::size_t size = 0;

    explicit Bytes(py::handle o) : info(py::reinterpret_borrow<py::buffer>(o).request())
    {
        std::size_t n = std::size_t(info.itemsize);
        for (py::ssize_t d : info.shape)
            n *= std::size_t(d);
        // Contiguity: each stride must be the product of the dimensions after it.
        py::ssize_t expect = info.itemsize;
        for (std::size_t i = info.shape.size(); i-- > 0;) {
            if (info.shape[i] > 1 && info.strides[i] != expect)
                throw py::type_error("buffer must be C-contiguous (use numpy.ascontiguousarray)");
            expect *= info.shape[i];
        }
        data = info.ptr;
        size = n;
    }
    nosql::Slice slice() const { return nosql::Slice(data, size); }
};

sql::Value fromPy(py::handle o)
{
    if (o.is_none())
        return sql::Value();
    if (PyBool_Check(o.ptr()))
        return sql::Value(o.ptr() == Py_True);
    if (PyLong_Check(o.ptr()))
        return sql::Value(o.cast<std::int64_t>());
    if (PyFloat_Check(o.ptr()))
        return sql::Value(o.cast<double>());
    if (PyUnicode_Check(o.ptr()))
        return sql::Value(o.cast<std::string>());
    if (PyBytes_Check(o.ptr())) {
        char* p = nullptr;
        Py_ssize_t n = 0;
        PyBytes_AsStringAndSize(o.ptr(), &p, &n);
        return sql::Value::blob(p, std::size_t(n));
    }
    if (isDatetimeLike(o))
        return sql::Value(sql::Datetime{usecFromDatetime(o)});
    // A numpy scalar (or 0-d array) exports the buffer protocol too, so it has
    // to be recognised before that path turns it into a BLOB.
    if (py::hasattr(o, "dtype") && py::hasattr(o, "ndim") && o.attr("ndim").cast<int>() == 0) {
        const std::string kind = py::str(o.attr("dtype").attr("kind"));
        if (kind == "b" || kind == "i" || kind == "u") {
            py::object i = py::reinterpret_steal<py::object>(PyNumber_Long(o.ptr()));
            if (!i)
                throw py::error_already_set();
            return sql::Value(i.cast<std::int64_t>());
        }
        if (kind == "f") {
            py::object f = py::reinterpret_steal<py::object>(PyNumber_Float(o.ptr()));
            if (!f)
                throw py::error_already_set();
            return sql::Value(f.cast<double>());
        }
    }
    if (PyObject_CheckBuffer(o.ptr())) {
        Bytes b(o);
        return sql::Value::blob(b.data, b.size);
    }
    // Whatever else answers as an integer, then as a float (Decimal, Fraction, ...).
    if (PyIndex_Check(o.ptr())) {
        py::object i = py::reinterpret_steal<py::object>(PyNumber_Index(o.ptr()));
        if (i)
            return sql::Value(i.cast<std::int64_t>());
        PyErr_Clear();
    }
    py::object f = py::reinterpret_steal<py::object>(PyNumber_Float(o.ptr()));
    if (f)
        return sql::Value(f.cast<double>());
    PyErr_Clear();
    throw py::type_error("cannot bind a " + std::string(py::str(py::type::of(o).attr("__name__"))) +
                         " as a SQL value");
}

py::object toPy(const sql::Value& v)
{
    switch (v.type()) {
    case sql::Type::Null: return py::none();
    case sql::Type::Integer: return py::int_(v.integer());
    case sql::Type::Real: return py::float_(v.real());
    case sql::Type::Datetime: return datetimeFromUsec(v.datetime().usec);
    case sql::Type::Text: {
        const std::string& s = v.text();
        PyObject* u = PyUnicode_DecodeUTF8(s.data(), py::ssize_t(s.size()), "surrogateescape");
        if (!u)
            throw py::error_already_set();
        return py::reinterpret_steal<py::object>(u);
    }
    case sql::Type::Blob: {
        const sql::Blob& b = v.blob();
        return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
    }
    }
    return py::none();
}

sql::Params paramsFrom(py::handle seq)
{
    sql::Params out;
    if (seq.is_none())
        return out;
    if (PyUnicode_Check(seq.ptr()) || PyBytes_Check(seq.ptr()))
        throw py::type_error("params must be a sequence of values, not a single str/bytes");
    for (py::handle item : py::reinterpret_borrow<py::sequence>(seq))
        out.push_back(fromPy(item));
    return out;
}

// ------------------------------------------------------------------ keys ---

std::string keyBytes(py::handle k)
{
    if (PyBytes_Check(k.ptr()))
        return k.cast<std::string>();
    if (PyUnicode_Check(k.ptr()))
        return k.cast<std::string>();
    if (PyLong_Check(k.ptr()) && !PyBool_Check(k.ptr())) {
        const unsigned long long v = PyLong_AsUnsignedLongLong(k.ptr());
        if (v == static_cast<unsigned long long>(-1) && PyErr_Occurred())
            throw py::error_already_set();  // OverflowError: negative, or above 2**64-1
        std::string out(8, '\0');
        for (int i = 0; i < 8; ++i)
            out[std::size_t(i)] = char(std::uint8_t(v >> (56 - 8 * i)));
        return out;
    }
    if (PyObject_CheckBuffer(k.ptr())) {
        Bytes b(k);
        return std::string(static_cast<const char*>(b.data), b.size);
    }
    throw py::type_error("a key must be bytes, str or a non-negative int");
}

/// The archive member name a key gets when the caller does not supply one:
/// the str itself, 16 hex digits for an int, hex for short bytes.
std::string defaultName(py::handle k, const std::string& bytes)
{
    if (PyUnicode_Check(k.ptr())) {
        if (bytes.empty() || bytes.size() > 100 || bytes.find('\0') != std::string::npos)
            throw py::value_error("a str key used as the member name must be 1..100 bytes; pass name=");
        return bytes;
    }
    static const char* hex = "0123456789abcdef";
    if (bytes.size() > 50)
        throw py::value_error("key is too long to name the member by its hex; pass name=");
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

std::vector<std::string> keyList(py::handle keys)
{
    std::vector<std::string> out;
    if (PyUnicode_Check(keys.ptr()) || PyBytes_Check(keys.ptr()))
        throw py::type_error("keys must be a sequence of keys, not a single key");
    for (py::handle k : py::reinterpret_borrow<py::iterable>(keys))
        out.push_back(keyBytes(k));
    return out;
}

std::vector<nosql::Slice> slices(const std::vector<std::string>& keys)
{
    std::vector<nosql::Slice> out;
    out.reserve(keys.size());
    for (const std::string& k : keys)
        out.emplace_back(k);
    return out;
}

/// Inverse of defaultName() for rebuild_index(key_of="int" | "hex").
std::string intKeyOf(std::string_view name)
{
    if (name.size() < 16)
        return std::string(name);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const char c = name[i];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return std::string(name);
        v = (v << 4) | std::uint64_t(d);
    }
    std::string out(8, '\0');
    for (int i = 0; i < 8; ++i)
        out[std::size_t(i)] = char(std::uint8_t(v >> (56 - 8 * i)));
    return out;
}

std::string hexKeyOf(std::string_view name)
{
    if (name.empty() || name.size() % 2)
        return std::string(name);
    std::string out;
    out.reserve(name.size() / 2);
    for (std::size_t i = 0; i < name.size(); i += 2) {
        int v = 0;
        for (std::size_t j = i; j < i + 2; ++j) {
            const char c = name[j];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return std::string(name);
            v = (v << 4) | d;
        }
        out.push_back(char(v));
    }
    return out;
}

// --------------------------------------------------------------- results ---

struct Columns
{
    std::vector<std::string> names;
    std::unordered_map<std::string, std::size_t> byLower;  ///< first column of each lowercased name
};

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Row
{
    std::shared_ptr<const Columns> cols;
    py::tuple values;

    std::size_t indexOf(py::handle key) const
    {
        if (PyLong_Check(key.ptr())) {
            auto i = key.cast<py::ssize_t>();
            const auto n = py::ssize_t(values.size());
            if (i < 0)
                i += n;
            if (i < 0 || i >= n)
                throw py::index_error("row index out of range");
            return std::size_t(i);
        }
        const std::string name = key.cast<std::string>();
        auto it = cols->byLower.find(lower(name));
        if (it == cols->byLower.end())
            throw py::key_error("no such column: " + name);
        return it->second;
    }
};

struct Result
{
    std::shared_ptr<const Columns> cols;
    py::list rows;  ///< of Row
    std::uint64_t changes = 0;
    std::int64_t lastInsertId = 0;
};

Result wrap(const sql::Result& r)
{
    auto cols = std::make_shared<Columns>();
    cols->names = r.columns();
    for (std::size_t i = 0; i < cols->names.size(); ++i)
        cols->byLower.emplace(lower(cols->names[i]), i);
    Result out;
    out.cols = cols;
    out.changes = r.changes();
    out.lastInsertId = r.lastInsertId();
    for (const sql::Row& row : r) {
        py::tuple t(row.size());
        for (std::size_t i = 0; i < row.size(); ++i)
            t[i] = toPy(row[i]);
        out.rows.append(Row{cols, std::move(t)});
    }
    return out;
}

// -------------------------------------------------------------- database ---

/// The Database plus the count of BlobStorage handles borrowing its Env, so
/// close() can refuse while one of them would be left pointing at nothing.
struct Holder
{
    sql::Database db;
    bool readOnly = false;
    std::mutex mtx;
    int attached = 0;            ///< live BlobStorage handles borrowing the Env
    bool closeRequested = false;

    /// close() from Python: the store is released now, or -- while a
    /// BlobStorage still borrows the Env -- when the last one goes, the way
    /// libsql's own statements and transactions keep a closed Database open.
    void requestClose()
    {
        std::lock_guard<std::mutex> lk(mtx);
        closeRequested = true;
        if (attached == 0)
            db.close();
    }
    void attach()
    {
        std::lock_guard<std::mutex> lk(mtx);
        ++attached;
    }
    void detach()
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (--attached == 0 && closeRequested)
            db.close();
    }
    bool closed()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return closeRequested || !db.valid();
    }
};

struct Database
{
    std::shared_ptr<Holder> h;

    sql::Database& db()
    {
        if (h->closed())
            throw py::value_error("the database is closed");
        return h->db;
    }
};

struct Transaction
{
    std::shared_ptr<Holder> h;
    sql::Transaction t;
    bool readOnly = false;

    sql::Transaction& live()
    {
        if (!t.valid()) {
            raise(gSqlError, "the transaction has already finished", "BadTransaction");
            throw py::error_already_set();
        }
        return t;
    }
};

struct Statement
{
    sql::Statement s;
};

Result execText(sql::Database& db, const std::string& text, py::handle params)
{
    const sql::Params p = paramsFrom(params);
    sql::Result r;
    {
        py::gil_scoped_release nogil;
        r = db.exec(text, p);
    }
    return wrap(r);
}

// ----------------------------------------------------------------- blobs ---

struct BlobStorage
{
    std::shared_ptr<Holder> h;
    nosql::BlobStorage store;
    bool readOnly = false;

    ~BlobStorage()
    {
        store = nosql::BlobStorage();  // before the Env it borrows can go
        if (h)
            h->detach();
    }
};

/// A read snapshot: its own read transaction, or a libsql Transaction's.
///
/// `keep`/`keepTxn` hold the Python objects this points into, and are dropped
/// on close so a finished Snapshot left in a variable pins nothing.
struct Snapshot
{
    BlobStorage* store = nullptr;
    nosql::Txn own;
    Transaction* borrowed = nullptr;
    py::object keep;
    py::object keepTxn;

    nosql::Txn& txn()
    {
        if (!store)
            throw py::value_error("the snapshot is closed");
        if (borrowed)
            return borrowed->live().txn();
        return own;
    }
    bool active() const { return store && (borrowed ? borrowed->t.valid() : own.valid()); }
    void close()
    {
        own = nosql::Txn();
        store = nullptr;
        borrowed = nullptr;
        keep = py::none();
        keepTxn = py::none();
    }
};

struct Writer
{
    BlobStorage* store = nullptr;
    nosql::BlobStorage::Writer w;
    Transaction* borrowed = nullptr;
    py::object keep;
    py::object keepTxn;

    bool active() const { return store != nullptr; }
    void check()
    {
        if (!store)
            throw py::value_error("the writer has already committed");
        if (borrowed)
            borrowed->live();  // raises if the transaction it writes into is gone
    }
    /// Commit, or abandon (which flushes the bytes but indexes nothing), then
    /// let go of everything this held.
    void finish(bool commit)
    {
        if (!store)
            return;
        if (commit) {
            check();
            py::gil_scoped_release nogil;
            w.commit();
        }
        w = nosql::BlobStorage::Writer();
        store = nullptr;
        borrowed = nullptr;
        keep = py::none();
        keepTxn = py::none();
    }
};

py::object blobOrNone(nosql::Blob b)
{
    if (!b.valid())
        return py::none();
    return py::cast(std::move(b));
}

py::list blobList(std::vector<nosql::Blob> blobs)
{
    py::list out(blobs.size());
    for (std::size_t i = 0; i < blobs.size(); ++i)
        out[i] = blobOrNone(std::move(blobs[i]));
    return out;
}

py::list keysOf(const nosql::BlobStorage& store, nosql::Txn& txn, py::handle prefix)
{
    const std::string pfx = prefix.is_none() ? std::string() : keyBytes(prefix);
    std::vector<std::string> keys;
    {
        py::gil_scoped_release nogil;
        store.forEachKey(txn, nosql::Slice(pfx), [&](nosql::Slice k) {
            keys.push_back(k.string());
            return true;
        });
    }
    py::list out(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i)
        out[i] = py::bytes(keys[i]);
    return out;
}

std::function<std::string(std::string_view)> keyOfFrom(py::handle keyOf)
{
    if (keyOf.is_none())
        return {};
    if (PyUnicode_Check(keyOf.ptr())) {
        const std::string mode = keyOf.cast<std::string>();
        if (mode == "int")
            return intKeyOf;
        if (mode == "hex")
            return hexKeyOf;
        throw py::value_error("key_of must be None, 'int', 'hex' or a callable");
    }
    py::object fn = py::reinterpret_borrow<py::object>(keyOf);
    return [fn](std::string_view name) {
        py::gil_scoped_acquire gil;
        return keyBytes(fn(py::str(name.data(), name.size())));
    };
}

nosql::BlobStorage::Access accessFrom(const std::string& s)
{
    if (s == "normal")
        return nosql::BlobStorage::Access::Normal;
    if (s == "random")
        return nosql::BlobStorage::Access::Random;
    if (s == "sequential")
        return nosql::BlobStorage::Access::Sequential;
    throw py::value_error("access must be 'normal', 'random' or 'sequential'");
}

}  // namespace

PYBIND11_MODULE(_libsql, m)
{
    m.doc() = "libsql: a small SQL engine over libnosql, with a zero-copy blob archive";
    m.attr("__version__") = sql::version();

    // --- exceptions ---------------------------------------------------------
    gError = py::handle(PyErr_NewException("libsql.Error", PyExc_Exception, nullptr));
    m.add_object("Error", gError);
    gSqlError = py::exception<sql::Error>(m, "SqlError", gError).release();
    gStoreError = py::exception<nosql::Error>(m, "StoreError", gError).release();
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p)
                std::rethrow_exception(p);
        } catch (const sql::Error& e) {
            raise(gSqlError, e.what(), codeName(e.code()));
        } catch (const nosql::Error& e) {
            raise(gStoreError, e.what(), codeName(e.code()));
        }
    });

    // --- results ------------------------------------------------------------
    py::class_<Row>(m, "Row", "One result row: index by position or by column name (case-insensitive).")
        .def("__len__", [](const Row& r) { return r.values.size(); })
        .def("__getitem__", [](const Row& r, py::handle key) { return py::object(r.values[r.indexOf(key)]); })
        .def("__iter__", [](const Row& r) { return py::iter(r.values); })
        .def("__contains__", [](const Row& r, const std::string& name) {
            return r.cols->byLower.count(lower(name)) != 0;
        })
        .def("get", [](const Row& r, py::handle key, py::object dflt) -> py::object {
            try {
                return py::object(r.values[r.indexOf(key)]);
            } catch (const py::key_error&) {
                return dflt;
            } catch (const py::index_error&) {
                return dflt;
            }
        }, py::arg("key"), py::arg("default") = py::none())
        .def("keys", [](const Row& r) { return r.cols->names; })
        .def("values", [](const Row& r) { return r.values; })
        .def("as_dict", [](const Row& r) {
            py::dict d;
            for (std::size_t i = 0; i < r.cols->names.size(); ++i)
                d[py::str(r.cols->names[i])] = r.values[i];
            return d;
        })
        .def("__repr__", [](const Row& r) {
            std::string s = "Row(";
            for (std::size_t i = 0; i < r.cols->names.size(); ++i) {
                if (i)
                    s += ", ";
                s += r.cols->names[i] + "=" + std::string(py::repr(r.values[i]));
            }
            return s + ")";
        });

    py::class_<Result>(m, "Result", "What a statement produced: rows for SELECT, counters otherwise.")
        .def_property_readonly("columns", [](const Result& r) { return r.cols->names; })
        .def_property_readonly("rows", [](const Result& r) {
            py::list out(r.rows.size());
            for (std::size_t i = 0; i < r.rows.size(); ++i) {
                const py::object row = r.rows[i];
                out[i] = row.cast<const Row&>().values;
            }
            return out;
        }, "The rows as plain tuples.")
        .def_property_readonly("changes", [](const Result& r) { return r.changes; },
                               "Rows inserted, updated or deleted.")
        .def_property_readonly("last_insert_id", [](const Result& r) { return r.lastInsertId; },
                               "Key INSERT gave the last row with an integer primary key; 0 otherwise.")
        .def("__len__", [](const Result& r) { return r.rows.size(); })
        .def("__getitem__", [](const Result& r, py::ssize_t i) {
            const auto n = py::ssize_t(r.rows.size());
            if (i < 0)
                i += n;
            if (i < 0 || i >= n)
                throw py::index_error("row index out of range");
            return py::object(r.rows[std::size_t(i)]);
        })
        .def("__iter__", [](const Result& r) { return py::iter(r.rows); })
        .def("first", [](const Result& r) -> py::object {
            return r.rows.empty() ? py::none() : py::object(r.rows[0]);
        }, "The first row, or None.")
        .def("scalar", [](const Result& r) -> py::object {
            if (r.rows.empty())
                return py::none();
            const py::object first = r.rows[0];
            const Row& row = first.cast<const Row&>();
            return row.values.empty() ? py::none() : py::object(row.values[0]);
        }, "The first column of the first row, or None: `db.exec('SELECT COUNT(*) FROM t').scalar()`.")
        .def("__repr__", [](const Result& r) {
            return "Result(" + std::to_string(r.rows.size()) + " rows, changes=" +
                   std::to_string(r.changes) + ")";
        });

    // --- transactions and statements --------------------------------------
    py::class_<Transaction>(m, "Transaction",
                            "A multi-statement transaction. As a context manager it commits on a clean "
                            "exit and rolls back on an exception.")
        .def("exec", [](Transaction& t, const std::string& text, py::handle params) {
            const sql::Params p = paramsFrom(params);
            sql::Transaction& txn = t.live();
            sql::Result r;
            {
                py::gil_scoped_release nogil;
                r = txn.exec(text, p);
            }
            return wrap(r);
        }, py::arg("sql"), py::arg("params") = py::none(),
           "Run every `;`-separated statement in `sql` and return the last one's result.")
        .def("commit", [](Transaction& t) {
            sql::Transaction& txn = t.live();
            py::gil_scoped_release nogil;
            txn.commit();
        })
        .def("rollback", [](Transaction& t) { t.t.rollback(); })
        .def_property_readonly("active", [](const Transaction& t) { return t.t.valid(); })
        .def_property_readonly("read_only", [](const Transaction& t) { return t.readOnly; })
        .def("__enter__", [](Transaction& t) -> Transaction& { return t; })
        .def("__exit__", [](Transaction& t, py::handle type, py::handle, py::handle) {
            if (!t.t.valid())
                return;
            if (type.is_none()) {
                py::gil_scoped_release nogil;
                t.t.commit();
            } else {
                t.t.rollback();
            }
        });

    py::class_<Statement>(m, "Statement",
                          "A statement parsed once and run as often as you like. Parameters are 1-based.")
        .def_property_readonly("sql", [](const Statement& s) { return s.s.sql(); })
        .def_property_readonly("parameters", [](const Statement& s) { return s.s.parameters(); },
                               "How many `?` placeholders the statement has.")
        .def_property_readonly("writes", [](const Statement& s) { return s.s.writes(); })
        .def("bind", [](Statement& s, std::size_t index, py::handle value) -> Statement& {
            s.s.bind(index, fromPy(value));
            return s;
        }, py::arg("index"), py::arg("value"), "Bind one parameter; `index` is 1-based.")
        .def("bind_all", [](Statement& s, py::handle params) -> Statement& {
            s.s.bind(paramsFrom(params));
            return s;
        }, py::arg("params"))
        .def("clear", [](Statement& s) -> Statement& {
            s.s.clear();
            return s;
        }, "Put every parameter back to NULL.")
        .def("exec", [](Statement& s, py::handle params, py::object txn) {
            if (!params.is_none())
                s.s.bind(paramsFrom(params));
            sql::Result r;
            if (txn.is_none()) {
                py::gil_scoped_release nogil;
                r = s.s.exec();
            } else {
                sql::Transaction& t = txn.cast<Transaction&>().live();
                py::gil_scoped_release nogil;
                r = s.s.exec(t);
            }
            return wrap(r);
        }, py::arg("params") = py::none(), py::arg("txn") = py::none(),
           "Run with the bindings in place (or `params`), in its own transaction or inside `txn`.");

    // --- database -----------------------------------------------------------
    py::class_<Database>(m, "Database", "One libnosql store holding the catalog, the tables and the indexes.")
        .def(py::init([](const std::string& path, bool readOnly, bool create, bool durable,
                         std::uint64_t maxSize) {
                 auto h = std::make_shared<Holder>();
                 h->readOnly = readOnly;
                 {
                     py::gil_scoped_release nogil;
                     h->db = sql::Database::configure()
                                 .readOnly(readOnly)
                                 .createIfMissing(create)
                                 .durable(durable)
                                 .maxSize(maxSize)
                                 .open(path);
                 }
                 return Database{std::move(h)};
             }),
             py::arg("path"), py::kw_only(), py::arg("read_only") = false, py::arg("create") = true,
             py::arg("durable") = true, py::arg("max_size") = std::uint64_t(64) << 30,
             "Open (creating unless `create=False`) the store at `path`. `durable=False` skips the "
             "fsync per commit for bulk loads. `max_size` is the hard ceiling on the file.")
        .def_property_readonly("path", [](Database& d) { return d.db().path().string(); })
        .def_property_readonly("read_only", [](const Database& d) { return d.h->readOnly; })
        .def_property_readonly("closed", [](const Database& d) { return d.h->closed(); })
        .def("exec", [](Database& d, const std::string& text, py::handle params) {
            return execText(d.db(), text, params);
        }, py::arg("sql"), py::arg("params") = py::none(),
           "Run every `;`-separated statement in one transaction; return the last one's result.")
        .def("prepare", [](Database& d, const std::string& text) {
            return Statement{d.db().prepare(text)};
        }, py::arg("sql"), "Parse now, run later; worth it for anything in a loop.")
        .def("begin", [](Database& d) {
            sql::Database& db = d.db();
            Transaction t{d.h, sql::Transaction(), false};
            py::gil_scoped_release nogil;
            t.t = db.begin();  // blocks while another write transaction is live
            return t;
        }, "A write transaction spanning several exec calls. Only one may be live at a time.")
        .def("begin_read", [](Database& d) {
            sql::Database& db = d.db();
            Transaction t{d.h, sql::Transaction(), true};
            py::gil_scoped_release nogil;
            t.t = db.beginRead();
            return t;
        }, "A snapshot for several reads: never blocks, never blocked, rejects writes.")
        .def("tables", [](Database& d) { return d.db().tables(); })
        .def("table", [](Database& d, const std::string& name) {
            const sql::TableInfo t = d.db().table(name);
            py::list cols;
            for (const sql::ColumnInfo& c : t.columns) {
                py::dict col;
                col["name"] = c.name;
                col["type"] = sql::toString(c.type);
                col["primary_key"] = c.primaryKey;
                col["not_null"] = c.notNull;
                col["unique"] = c.unique;
                col["default"] = c.hasDefault ? toPy(c.defaultValue) : py::none();
                cols.append(col);
            }
            py::dict out;
            out["name"] = t.name;
            out["columns"] = cols;
            py::list key;
            for (const int c : t.primaryKey)
                key.append(t.columns[std::size_t(c)].name);
            out["primary_key"] = key;
            return out;
        }, py::arg("name"),
        "The table's schema: its name, a dict per column, and 'primary_key', the key's column "
        "names in key order (empty for a table keyed by its implicit rowid).")
        .def("indexes", [](Database& d, py::object table) {
            py::list out;
            for (const sql::IndexInfo& ix : d.db().indexes(table.is_none() ? std::string() : table.cast<std::string>())) {
                py::dict e;
                e["name"] = ix.name;
                e["table"] = ix.table;
                e["columns"] = ix.columns;
                e["unique"] = ix.unique;
                out.append(e);
            }
            return out;
        }, py::arg("table") = py::none())
        .def("close", [](Database& d) { d.h->requestClose(); },
             "Release the store. Statements, transactions and BlobStorage objects already created "
             "keep it open until they go; this handle refuses further work at once.")
        .def("__enter__", [](Database& d) -> Database& { return d; })
        .def("__exit__", [](Database& d, py::handle, py::handle, py::handle) { d.h->requestClose(); })
        .def("__repr__", [](const Database& d) {
            return !d.h->closed() ? "Database('" + d.h->db.path().string() + "')" : std::string("Database(closed)");
        });

    // --- blobs --------------------------------------------------------------
    py::class_<nosql::Blob>(m, "Blob", py::buffer_protocol(),
                            "A zero-copy view of one blob. Exports the buffer protocol over the archive "
                            "mapping it pins: memoryview(blob), numpy.frombuffer(blob, dtype) and "
                            "torch.frombuffer(blob, dtype=...) read the bytes in place. Keep the Blob (or "
                            "the array's .base) alive for as long as you use the view.")
        .def_buffer([](nosql::Blob& b) {
            return py::buffer_info(const_cast<std::byte*>(b.data().data()), 1, "B", 1,
                                   {py::ssize_t(b.size())}, {py::ssize_t(1)}, true);
        })
        .def_property_readonly("name", [](const nosql::Blob& b) { return std::string(b.name()); },
                               "The tar member name.")
        .def_property_readonly("size", [](const nosql::Blob& b) { return b.size(); })
        .def_property_readonly("offset", [](const nosql::Blob& b) { return b.offset(); },
                               "Payload offset inside the archive file, 512-aligned.")
        .def_property_readonly("checksum", [](const nosql::Blob& b) { return b.storedChecksum(); })
        .def_property_readonly("has_checksum", [](const nosql::Blob& b) { return b.hasChecksum(); })
        .def("verify", [](const nosql::Blob& b) {
            py::gil_scoped_release nogil;
            return b.verify();
        }, "Recompute the XXH64 over the payload and compare. True when none was recorded.")
        .def("tobytes", [](const nosql::Blob& b) {
            return py::bytes(reinterpret_cast<const char*>(b.data().data()), b.size());
        }, "A copy, as bytes.")
        .def("__len__", [](const nosql::Blob& b) { return b.size(); })
        .def("__repr__", [](const nosql::Blob& b) {
            return "Blob(name='" + std::string(b.name()) + "', size=" + std::to_string(b.size()) + ")";
        });

    py::class_<Writer>(m, "Writer",
                       "Batches many appends behind one index commit. As a context manager it commits "
                       "on a clean exit; on an exception the bytes stay in the archive unindexed, for the "
                       "next catch_up().")
        .def("add", [](Writer& w, py::handle key, py::handle data, py::object name) {
            w.check();
            const std::string k = keyBytes(key);
            const std::string n = name.is_none() ? defaultName(key, k) : name.cast<std::string>();
            if (PyUnicode_Check(data.ptr())) {
                const std::string s = data.cast<std::string>();
                py::gil_scoped_release nogil;
                w.w.add(k, n, nosql::Slice(s));
            } else {
                Bytes b(data);
                py::gil_scoped_release nogil;
                w.w.add(k, n, b.slice());
            }
        }, py::arg("key"), py::arg("data"), py::arg("name") = py::none(),
           "Queue `data` (bytes, str, or any C-contiguous buffer such as a numpy array) under `key`. "
           "`name` is the tar member name, 1..100 bytes; derived from the key when omitted.")
        .def("commit", [](Writer& w) { w.finish(true); },
             "Flush, terminate the archive and commit the index (or record it in the joined "
             "transaction). Idempotent.")
        .def("abandon", [](Writer& w) { w.finish(false); },
             "Give up without indexing. The bytes already written stay in the archive for catch_up().")
        .def_property_readonly("active", [](const Writer& w) { return w.active(); })
        .def("__enter__", [](Writer& w) -> Writer& { return w; })
        .def("__exit__", [](Writer& w, py::handle type, py::handle, py::handle) {
            w.finish(type.is_none());
        });

    py::class_<Snapshot>(m, "Snapshot",
                         "One read snapshot reused across many lookups: the form for a loader loop, where "
                         "a transaction per sample would dominate. Holding it pins the snapshot, so keep "
                         "it for a batch or an epoch, not forever while a writer is busy.")
        .def("get", [](Snapshot& s, py::handle key, py::object dflt) -> py::object {
            const std::string k = keyBytes(key);
            nosql::Txn& t = s.txn();
            nosql::Blob b;
            {
                py::gil_scoped_release nogil;
                b = s.store->store.find(t, k);
            }
            return b.valid() ? py::cast(std::move(b)) : dflt;
        }, py::arg("key"), py::arg("default") = py::none())
        .def("__getitem__", [](Snapshot& s, py::handle key) {
            const std::string k = keyBytes(key);
            nosql::Txn& t = s.txn();
            nosql::Blob b;
            {
                py::gil_scoped_release nogil;
                b = s.store->store.find(t, k);
            }
            if (!b.valid())
                throw py::key_error(std::string(py::repr(key)));
            return b;
        })
        .def("__contains__", [](Snapshot& s, py::handle key) {
            const std::string k = keyBytes(key);
            nosql::Txn& t = s.txn();
            py::gil_scoped_release nogil;
            return s.store->store.find(t, k).valid();
        })
        .def("get_many", [](Snapshot& s, py::handle keys) {
            const std::vector<std::string> ks = keyList(keys);
            nosql::Txn& t = s.txn();
            std::vector<nosql::Blob> out;
            {
                py::gil_scoped_release nogil;
                out = s.store->store.findMany(t, slices(ks));
            }
            return blobList(std::move(out));
        }, py::arg("keys"), "Look a batch up in one pass; None where a key is absent, input order kept.")
        .def("prefetch", [](Snapshot& s, py::handle keys) {
            const std::vector<std::string> ks = keyList(keys);
            nosql::Txn& t = s.txn();
            const std::vector<nosql::Slice> sl = slices(ks);
            py::gil_scoped_release nogil;
            return s.store->store.prefetch(t, sl.data(), sl.size());
        }, py::arg("keys"), "Start reading these payloads now, without waiting. Returns how many keys exist.")
        .def("keys", [](Snapshot& s, py::handle prefix) {
            return keysOf(s.store->store, s.txn(), prefix);
        }, py::arg("prefix") = py::none(), "Every key in key order, as bytes; optionally only those with `prefix`.")
        .def("close", [](Snapshot& s) { s.close(); })
        .def_property_readonly("active", [](const Snapshot& s) { return s.active(); })
        .def("__enter__", [](Snapshot& s) -> Snapshot& { return s; })
        .def("__exit__", [](Snapshot& s, py::handle, py::handle, py::handle) { s.close(); });

    py::class_<BlobStorage>(m, "BlobStorage",
                            "Bulk payloads in a tar archive beside the database, indexed by a sub-database "
                            "in it. Reads are zero-copy; writes coalesce; lookups batch.")
        .def(py::init([](Database& db, const std::string& name, py::object directory, py::object fileName,
                         py::object readOnly, bool syncOnAppend, bool catchUp, const std::string& access,
                         std::size_t writeBuffer) {
                 if (name.empty() || name == "sql_catalog" || name.rfind("tbl:", 0) == 0 ||
                     name.rfind("idx:", 0) == 0)
                     throw py::value_error("'" + name + "' is reserved by libsql; pick another index name");
                 nosql::BlobStorage::Options opts;
                 if (!directory.is_none())
                     opts.directory(directory.cast<std::string>());
                 if (!fileName.is_none())
                     opts.fileName(fileName.cast<std::string>());
                 const bool ro = readOnly.is_none() ? db.h->readOnly : readOnly.cast<bool>();
                 opts.readOnly(ro).syncOnAppend(syncOnAppend).catchUpOnOpen(catchUp)
                     .accessPattern(accessFrom(access)).writeBuffer(writeBuffer);
                 nosql::Env& env = db.db().env();
                 auto out = std::make_unique<BlobStorage>();
                 out->h = db.h;
                 out->readOnly = ro;
                 db.h->attach();
                 {
                     py::gil_scoped_release nogil;
                     out->store = nosql::BlobStorage::open(env, nosql::Slice(name), opts);
                 }
                 return out;
             }),
             py::arg("db"), py::arg("name") = "blobs", py::kw_only(), py::arg("directory") = py::none(),
             py::arg("file_name") = py::none(), py::arg("read_only") = py::none(),
             py::arg("sync_on_append") = false, py::arg("catch_up_on_open") = true,
             py::arg("access") = "normal", py::arg("write_buffer") = std::size_t(4) << 20,
             py::keep_alive<1, 2>(),
             "Open (creating if needed) the archive for index sub-database `name` -- `<name>.tar` beside "
             "the store unless `directory`/`file_name` say otherwise. `read_only` defaults to the "
             "database's. `access` is 'normal', 'random' (shuffled training reads) or 'sequential'. "
             "`write_buffer` is how much a Writer queues before writing (0 writes through).")
        .def_property_readonly("archive_path", [](const BlobStorage& b) { return b.store.archivePath().string(); })
        .def_property_readonly("archive_size", [](const BlobStorage& b) { return b.store.archiveSize(); },
                               "The append point: end of the last member, excluding the end marker.")
        .def_property_readonly("indexed_up_to", [](const BlobStorage& b) {
            py::gil_scoped_release nogil;
            return b.store.indexedUpTo();
        })
        .def_property_readonly("read_only", [](const BlobStorage& b) { return b.readOnly; })
        .def("__len__", [](const BlobStorage& b) {
            py::gil_scoped_release nogil;
            return b.store.count();
        })
        .def("put", [](BlobStorage& b, py::handle key, py::handle data, py::object name) {
            const std::string k = keyBytes(key);
            const std::string n = name.is_none() ? defaultName(key, k) : name.cast<std::string>();
            if (PyUnicode_Check(data.ptr())) {
                const std::string s = data.cast<std::string>();
                py::gil_scoped_release nogil;
                b.store.put(k, n, nosql::Slice(s));
            } else {
                Bytes bytes(data);
                py::gil_scoped_release nogil;
                b.store.put(k, n, bytes.slice());
            }
        }, py::arg("key"), py::arg("data"), py::arg("name") = py::none(),
           "Append one member and commit. One transaction per call; use writer() for many.")
        .def("writer", [](py::object self, py::object txn) {
            BlobStorage& b = self.cast<BlobStorage&>();
            auto w = std::make_unique<Writer>();
            w->store = &b;
            w->keep = self;
            if (txn.is_none()) {
                py::gil_scoped_release nogil;
                w->w = b.store.beginWrite();  // blocks while another write transaction is live
            } else {
                Transaction& t = txn.cast<Transaction&>();
                if (t.readOnly)
                    throw py::value_error("a Writer needs a write transaction (Database.begin())");
                w->borrowed = &t;
                w->keepTxn = txn;
                w->w = b.store.beginWrite(t.live().txn());
            }
            return w;
        }, py::arg("txn") = py::none(),
           "A Writer holding the store's write transaction -- or joining `txn`, a Database.begin() "
           "transaction, so blobs and the rows that describe them commit together.")
        .def("get", [](BlobStorage& b, py::handle key, py::object dflt) -> py::object {
            const std::string k = keyBytes(key);
            nosql::Blob blob;
            {
                py::gil_scoped_release nogil;
                blob = b.store.find(k);
            }
            return blob.valid() ? py::cast(std::move(blob)) : dflt;
        }, py::arg("key"), py::arg("default") = py::none(),
           "Zero-copy lookup in a snapshot of its own; `default` when absent.")
        .def("__getitem__", [](BlobStorage& b, py::handle key) {
            const std::string k = keyBytes(key);
            nosql::Blob blob;
            {
                py::gil_scoped_release nogil;
                blob = b.store.find(k);
            }
            if (!blob.valid())
                throw py::key_error(std::string(py::repr(key)));
            return blob;
        })
        .def("__contains__", [](BlobStorage& b, py::handle key) {
            const std::string k = keyBytes(key);
            py::gil_scoped_release nogil;
            return b.store.contains(k);
        })
        .def("get_many", [](BlobStorage& b, py::handle keys) {
            const std::vector<std::string> ks = keyList(keys);
            std::vector<nosql::Blob> out;
            {
                py::gil_scoped_release nogil;
                out = b.store.findMany(slices(ks));
            }
            return blobList(std::move(out));
        }, py::arg("keys"), "Look a batch up against one snapshot; None where a key is absent.")
        .def("prefetch", [](BlobStorage& b, py::handle keys) {
            const std::vector<std::string> ks = keyList(keys);
            const std::vector<nosql::Slice> sl = slices(ks);
            nosql::Env& env = b.h->db.env();
            py::gil_scoped_release nogil;
            return env.read([&](nosql::Txn& t) { return b.store.prefetch(t, sl.data(), sl.size()); });
        }, py::arg("keys"),
           "Start reading these payloads into memory now and return at once. Call it with the next "
           "batch's keys while the current batch is being consumed. Returns how many keys exist.")
        .def("warm", [](BlobStorage& b) {
            py::gil_scoped_release nogil;
            b.store.warm();
        }, "Ask the OS to read the whole archive into memory, without waiting for it.")
        .def("keys", [](BlobStorage& b, py::handle prefix) {
            return b.h->db.env().read([&](nosql::Txn& t) { return keysOf(b.store, t, prefix); });
        }, py::arg("prefix") = py::none(), "Every key in key order, as bytes.")
        .def("snapshot", [](py::object self, py::object txn) {
            BlobStorage& b = self.cast<BlobStorage&>();
            auto s = std::make_unique<Snapshot>();
            s->store = &b;
            s->keep = self;
            if (txn.is_none()) {
                s->own = b.h->db.env().readTxn();
            } else {
                s->borrowed = &txn.cast<Transaction&>();
                s->keepTxn = txn;
            }
            return s;
        }, py::arg("txn") = py::none(),
           "A read snapshot for many lookups -- its own, or the one a Database transaction holds, "
           "so SQL rows and the blobs they point at are read from the same instant.")
        .def("erase", [](BlobStorage& b, py::handle key) {
            const std::string k = keyBytes(key);
            py::gil_scoped_release nogil;
            return b.store.erase(k);
        }, py::arg("key"), "Drop the index entry. The member stays in the archive.")
        .def("finalize", [](BlobStorage& b) {
            py::gil_scoped_release nogil;
            b.store.finalize();
        }, "Terminate and pad the archive for tar(1), and fsync it.")
        .def("sync", [](BlobStorage& b) {
            py::gil_scoped_release nogil;
            b.store.sync();
        })
        .def("catch_up", [](BlobStorage& b) {
            py::gil_scoped_release nogil;
            return b.store.catchUp();
        }, "Index members appended since the index last looked; returns how many.")
        .def("rebuild_index", [](BlobStorage& b, py::object keyOf) {
            const auto fn = keyOfFrom(keyOf);
            py::gil_scoped_release nogil;
            return b.store.rebuildIndex(fn);
        }, py::arg("key_of") = py::none(),
           "Rebuild the index from the archive's headers. `key_of` maps a member name back to a key: "
           "None uses the name, 'int' undoes the default naming of int keys, 'hex' that of bytes keys, "
           "or pass a callable.")
        .def("list", [](const BlobStorage& b) {
            std::vector<nosql::TarEntry> entries;
            {
                py::gil_scoped_release nogil;
                entries = b.store.list();
            }
            py::list out;
            for (const nosql::TarEntry& e : entries)
                out.append(py::make_tuple(e.name, e.offset, e.size));
            return out;
        }, "What is physically in the archive, as (name, offset, size), ignoring the index.")
        .def("__repr__", [](const BlobStorage& b) {
            return "BlobStorage('" + b.store.archivePath().string() + "')";
        });
}
