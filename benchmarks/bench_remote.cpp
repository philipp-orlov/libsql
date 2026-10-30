// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Remote databases over loopback: how long a request takes, how many rows per second a result streams, what many
// sessions do together, and what the server weighs with many sessions or a long run. Run in Release:
//
//   ./bench_remote [soak-seconds]       (default 20)

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "http/server.hpp"
#include "sql/remote/sql_socket.hpp"

#include "bench_util.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::uint16_t freePort()
{
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t n = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &n);
    ::close(s);
    return ntohs(a.sin_port);
}

long residentKiB()
{
    long pages = 0, resident = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

struct Served
{
    bench::Scratch dir{"remote"};
    sql::Database db;
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::unique_ptr<sql::remote::SqlSocket> socket;
    std::future<void> serving;
    std::string localPath;

    explicit Served(unsigned workers = 4) : db(sql::Database::configure().busyTimeout(5s).open(dir.file("db")))
    {
        config.host = "127.0.0.1";
        config.port = freePort();
        config.ioThreads = 2;
        config.workerThreads = 1;
        config.maxConnectionsPerLoop = 4096;
        server = std::make_unique<http::Server>(config);
        socket = std::make_unique<sql::remote::SqlSocket>(db, sql::remote::SqlSocketOptions().workers(workers).maxSessions(4096));
        socket->attach(*server, "/api/v1/ws/sql");
        localPath = "/tmp/libsql-bench-" + std::to_string(::getpid()) + ".sock";
        socket->listenLocal(localPath);
        serving = std::async(std::launch::async, [this] { server->run(); });
        std::this_thread::sleep_for(300ms);
    }
    ~Served()
    {
        socket->shutdown(500ms);
        server->stop();
        serving.get();
    }
    /// The same database through the local socket (shared-memory rings).
    sql::Database connectLocal(const sql::RemoteOptions& o = sql::RemoteOptions()) const
    {
        return sql::Database::connect("unix://" + localPath, o);
    }
    sql::Database connect(const sql::RemoteOptions& o = sql::RemoteOptions()) const
    {
        return sql::Database::connect("ws://127.0.0.1:" + std::to_string(config.port) + "/api/v1/ws/sql", o);
    }
};

double percentile(std::vector<double>& v, double p)
{
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(p * v.size()))];
}

void roundTrip(Served& s)
{
    s.db.exec("CREATE TABLE one(a INTEGER PRIMARY KEY, b TEXT)");
    s.db.exec("INSERT INTO one VALUES (1, 'x')");
    sql::Database remote = s.connect();
    sql::Statement local = s.db.prepare("SELECT a, b FROM one WHERE a = ?");
    sql::Statement far = remote.prepare("SELECT a, b FROM one WHERE a = ?");
    sql::Database nearby = s.connectLocal();
    sql::Statement near = nearby.prepare("SELECT a, b FROM one WHERE a = ?");
    struct Sink final : sql::ResultConsumer
    {
        void begin(const sql::ResultShape&) override {}
        bool row(const sql::RowView&) override { return true; }
        void end(std::uint64_t, std::int64_t) override {}
    } sink;
    auto measure = [&](sql::Statement& st, int n) {
        std::vector<double> us;
        us.reserve(n);
        st.bind(1, sql::Value(1));
        for (int i = 0; i < 2000; ++i)
            st.exec(sink);
        for (int i = 0; i < n; ++i) {
            const auto a = Clock::now();
            st.exec(sink);
            us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
        }
        return us;
    };
    auto l = measure(local, 20000), r = measure(far, 20000), n = measure(near, 20000);
    std::printf("round trip, prepared point query (20,000 each)\n");
    std::printf("  local   median %7.1f us   p99 %7.1f us\n", percentile(l, 0.5), percentile(l, 0.99));
    std::printf("  remote  median %7.1f us   p99 %7.1f us   (loopback WebSocket)\n", percentile(r, 0.5), percentile(r, 0.99));
    std::printf("  unix    median %7.1f us   p99 %7.1f us   (local socket, shared memory)\n\n", percentile(n, 0.5), percentile(n, 0.99));
}

void throughput(Served& s, int rows)
{
    s.db.exec("CREATE TABLE orders(id INTEGER PRIMARY KEY, name TEXT, status TEXT, price REAL, qty INTEGER, created DATETIME)");
    {
        std::mt19937_64 rng(7);
        sql::Transaction txn = s.db.begin();
        sql::Statement st = s.db.prepare("INSERT INTO orders VALUES (?,?,?,?,?,?)");
        const char* statuses[] = {"new", "paid", "packed", "shipped", "returned"};
        for (int i = 0; i < rows; ++i)
            st.exec(txn, sql::Params{sql::Value(i), sql::Value("customer-" + std::to_string(rng() % 100000)), sql::Value(statuses[rng() % 5]),
                                     sql::Value(double(rng() % 10000) / 100.0), sql::Value(std::int64_t(rng() % 1000)),
                                     sql::Value(sql::Datetime{(1700000000LL + std::int64_t(rng() % 31536000)) * 1000000})});
        txn.commit();
    }
    sql::Database remote = s.connect(sql::RemoteOptions().window(4 << 20));
    struct Count final : sql::ResultConsumer
    {
        std::size_t rows = 0;
        std::int64_t sum = 0;
        void begin(const sql::ResultShape&) override {}
        bool row(const sql::RowView& r) override
        {
            ++rows;
            sum += r[4].integer();
            return true;
        }
        void end(std::uint64_t, std::int64_t) override {}
    };
    auto time = [&](sql::Database& db) {
        Count c;
        const auto a = Clock::now();
        db.exec("SELECT id, name, status, price, qty, created FROM orders", sql::Params{}, c);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        return std::pair(ms, c.rows);
    };
    time(s.db);
    time(remote);
    auto [lms, lrows] = time(s.db);
    auto [rms, rrows] = time(remote);
    std::printf("streaming %d rows x 6 columns through a consumer\n", rows);
    std::printf("  local   %8.1f ms  %9.0f rows/s\n", lms, lrows / lms * 1000);
    std::printf("  remote  %8.1f ms  %9.0f rows/s\n\n", rms, rrows / rms * 1000);
}

void concurrency(Served& s)
{
    std::printf("concurrent sessions, prepared point queries for 3 s each (4 workers)\n");
    for (int sessions : {1, 8, 64}) {
        std::atomic<long> requests{0};
        std::atomic<bool> stop{false};
        std::vector<std::vector<double>> latencies(sessions);
        std::vector<std::thread> threads;
        for (int i = 0; i < sessions; ++i) {
            threads.emplace_back([&, i] {
                sql::Database db = s.connect();
                sql::Statement st = db.prepare("SELECT a, b FROM one WHERE a = ?");
                st.bind(1, sql::Value(1));
                struct Sink final : sql::ResultConsumer
                {
                    void begin(const sql::ResultShape&) override {}
                    bool row(const sql::RowView&) override { return true; }
                    void end(std::uint64_t, std::int64_t) override {}
                } sink;
                while (!stop.load()) {
                    const auto a = Clock::now();
                    st.exec(sink);
                    latencies[i].push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
                    ++requests;
                }
            });
        }
        std::this_thread::sleep_for(3s);
        stop = true;
        for (auto& t : threads)
            t.join();
        std::vector<double> all;
        for (auto& l : latencies)
            all.insert(all.end(), l.begin(), l.end());
        std::printf("  %2d sessions  %8.0f requests/s   median %7.1f us   p99 %8.1f us\n", sessions, requests.load() / 3.0,
                    percentile(all, 0.5), percentile(all, 0.99));
    }
    std::printf("\n");
}

void memory(Served& s)
{
    const long base = residentKiB();
    std::vector<sql::Database> idle;
    for (int i = 0; i < 256; ++i)
        idle.push_back(s.connect());
    for (auto& db : idle)
        db.exec("SELECT 1");
    const long withIdle = residentKiB();
    std::printf("memory (this process holds both the server and the clients)\n");
    std::printf("  256 sessions that each ran one query: +%ld KiB resident (about %ld KiB per session, both ends)\n", withIdle - base,
                (withIdle - base) / 256);
    idle.clear();
    std::this_thread::sleep_for(500ms);
    std::printf("  after they closed: +%ld KiB over the start\n\n", residentKiB() - base);
}

void soak(Served& s, int seconds)
{
    sql::Database db = s.connect();
    sql::Statement st = db.prepare("SELECT id, name, qty FROM orders WHERE id < ?");
    st.bind(1, sql::Value(50));
    struct Sink final : sql::ResultConsumer
    {
        void begin(const sql::ResultShape&) override {}
        bool row(const sql::RowView&) override { return true; }
        void end(std::uint64_t, std::int64_t) override {}
    } sink;
    std::printf("soak: %d s of mixed requests on 4 sessions; resident memory sampled every second\n", seconds);
    std::atomic<bool> stop{false};
    std::atomic<long> requests{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            sql::Database own = s.connect();
            sql::Statement own_st = own.prepare("SELECT id, name, qty FROM orders WHERE id < ?");
            long n = 0;
            while (!stop.load()) {
                own_st.bind(1, sql::Value(1 + (n++ % 200)));
                own_st.exec(sink);
                if (n % 500 == 0) {
                    sql::Transaction t = own.beginRead();
                    t.exec("SELECT count(*) FROM orders WHERE qty > 500");
                }
                ++requests;
            }
        });
    }
    long first = 0, last = 0;
    for (int t = 1; t <= seconds; ++t) {
        std::this_thread::sleep_for(1s);
        const long rss = residentKiB();
        if (t == std::min(5, seconds))
            first = rss;
        last = rss;
        if (t % 5 == 0)
            std::printf("  t=%3d s  %9ld requests  resident %ld KiB\n", t, requests.load(), rss);
    }
    stop = true;
    for (auto& t : threads)
        t.join();
    std::printf("  %ld requests; resident grew %ld KiB after the warm-up\n\n", requests.load(), last - first);
}

}  // namespace

int main(int argc, char** argv)
{
    const int soakSeconds = argc > 1 ? std::atoi(argv[1]) : 20;
    Served s;
    roundTrip(s);
    throughput(s, 100000);
    concurrency(s);
    memory(s);
    soak(s, soakSeconds);
    return 0;
}
