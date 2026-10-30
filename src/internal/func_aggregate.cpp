// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// SQLite's built-in aggregate functions: count, sum, total, avg, min, max,
// group_concat and string_agg. Each can also run as a window function; all
// but min and max can be inverted for sliding frames.

#include <cmath>
#include <deque>
#include <limits>

#include "internal/func.hpp"

namespace sql::internal {

namespace {

class Count final : public AggregateState
{
public:
    void step(FuncContext&, int argc, Args argv) override
    {
        if (argc == 0 || !argv[0]->isNull())
            ++n_;
    }
    void inverse(FuncContext&, int argc, Args argv) override
    {
        if (argc == 0 || !argv[0]->isNull())
            --n_;
    }
    void value(FuncContext&, Value& out) override { out.setInteger(n_); }
    void reset() override { n_ = 0; }

private:
    std::int64_t n_ = 0;
};

/// sum/total/avg, with SQLite 3.45's arithmetic: exact integers until a real
/// or an overflow appears, then Kahan-Babuska-Neumaier compensated doubles.
class Sum final : public AggregateState
{
public:
    enum class Mode
    {
        Sum,
        Total,
        Avg,
    };
    explicit Sum(Mode m) : mode_(m) {}

    void step(FuncContext&, int, Args argv) override { add(*argv[0], 1); }
    void inverse(FuncContext&, int, Args argv) override { add(*argv[0], -1); }

    void value(FuncContext& ctx, Value& out) override
    {
        switch (mode_) {
            case Mode::Sum:
                if (cnt_ <= 0) {
                    out.setNull();
                    return;
                }
                if (approx_) {
                    if (overflow_)
                        ctx.error("integer overflow");
                    out.setReal(std::isinf(err_) || std::isnan(err_) ? sum_ : sum_ + err_);
                } else {
                    out.setInteger(isum_);
                }
                return;
            case Mode::Total:
                if (approx_)
                    out.setReal(std::isinf(err_) || std::isnan(err_) ? sum_ : sum_ + err_);
                else
                    out.setReal(double(isum_));
                return;
            case Mode::Avg: {
                if (cnt_ <= 0) {
                    out.setNull();
                    return;
                }
                double r;
                if (approx_) {
                    r = sum_;
                    if (!(std::isinf(err_) || std::isnan(err_)))
                        r += err_;
                } else {
                    r = double(isum_);
                }
                out.setReal(r / double(cnt_));
                return;
            }
        }
    }

    void reset() override
    {
        cnt_ = 0;
        isum_ = 0;
        sum_ = err_ = 0;
        approx_ = overflow_ = false;
    }

private:
    void kbn(double r)
    {
        const volatile double s = sum_;
        const volatile double t = s + r;
        if (std::fabs(s) > std::fabs(r))
            err_ += (s - t) + r;
        else
            err_ += (r - t) + s;
        sum_ = t;
    }
    void kbnInt(std::int64_t v)
    {
        if (v <= -4503599627370496LL || v >= 4503599627370496LL) {
            const std::int64_t big = v % 16384;
            kbn(double(v - big));
            kbn(double(big));
        } else {
            kbn(double(v));
        }
    }
    void enterApprox()
    {
        approx_ = true;
        const std::int64_t i = isum_;
        sum_ = err_ = 0;
        kbnInt(i);
    }

    void add(const Value& v, int sign)
    {
        if (v.isNull())
            return;
        cnt_ += sign;
        if (v.type() == Type::Integer || v.type() == Type::Datetime) {
            std::int64_t x = v.type() == Type::Integer ? v.integer() : v.datetime().usec;
            if (!approx_) {
                std::int64_t r;
                const bool ovf = sign > 0 ? __builtin_add_overflow(isum_, x, &r)
                                          : __builtin_sub_overflow(isum_, x, &r);
                if (!ovf) {
                    isum_ = r;
                    return;
                }
                overflow_ = true;
                enterApprox();
            }
            if (sign < 0) {
                if (x == std::numeric_limits<std::int64_t>::min()) {
                    kbn(9223372036854775808.0);
                    return;
                }
                x = -x;
            }
            kbnInt(x);
            return;
        }
        if (!approx_)
            enterApprox();
        kbn(sign * toDouble(v));
    }

    Mode mode_;
    std::int64_t cnt_ = 0;
    std::int64_t isum_ = 0;
    double sum_ = 0, err_ = 0;
    bool approx_ = false, overflow_ = false;
};

class MinMax final : public AggregateState
{
public:
    explicit MinMax(bool max) : max_(max) {}
    void step(FuncContext& ctx, int, Args argv) override
    {
        const Value& v = *argv[0];
        if (v.isNull())
            return;
        if (!have_) {
            best_ = v;
            have_ = true;
            return;
        }
        const int c = compareValues(v, best_, ctx.collation);
        if (max_ ? c > 0 : c < 0)
            best_ = v;
    }
    void value(FuncContext&, Value& out) override
    {
        if (have_)
            out = best_;
        else
            out.setNull();
    }
    void reset() override { have_ = false; }

private:
    bool max_;
    bool have_ = false;
    Value best_;
};

/// group_concat / string_agg. Each piece's length is remembered so that a
/// sliding window frame can drop pieces from the front.
class GroupConcat final : public AggregateState
{
public:
    explicit GroupConcat(bool requireSep) : requireSep_(requireSep) {}

    void step(FuncContext&, int argc, Args argv) override
    {
        const Value& v = *argv[0];
        if (v.isNull())
            return;
        std::size_t sepLen = 0;
        if (count_ > 0) {
            const std::size_t before = text_.size();
            if (argc > 1) {
                if (!argv[1]->isNull())
                    appendText(*argv[1], text_);
            } else if (!requireSep_) {
                text_ += ',';
            }
            sepLen = text_.size() - before;
        }
        const std::size_t before = text_.size();
        appendText(v, text_);
        pieces_.push_back({sepLen, text_.size() - before});
        ++count_;
    }

    void inverse(FuncContext&, int, Args argv) override
    {
        if (argv[0]->isNull() || pieces_.empty())
            return;
        // Drop the first piece and the separator that follows it.
        const Piece first = pieces_.front();
        pieces_.pop_front();
        std::size_t drop = first.len;
        if (!pieces_.empty()) {
            drop += pieces_.front().sep;
            pieces_.front().sep = 0;
        }
        text_.erase(0, drop);
        --count_;
    }

    void value(FuncContext&, Value& out) override
    {
        if (count_ == 0)
            out.setNull();
        else
            out.setText(text_);
    }

    void reset() override
    {
        text_.clear();
        pieces_.clear();
        count_ = 0;
    }

private:
    struct Piece
    {
        std::size_t sep, len;
    };
    bool requireSep_;
    std::string text_;
    std::deque<Piece> pieces_;
    std::int64_t count_ = 0;
};

std::unique_ptr<AggregateState> makeCount()
{
    return std::make_unique<Count>();
}
std::unique_ptr<AggregateState> makeSum()
{
    return std::make_unique<Sum>(Sum::Mode::Sum);
}
std::unique_ptr<AggregateState> makeTotal()
{
    return std::make_unique<Sum>(Sum::Mode::Total);
}
std::unique_ptr<AggregateState> makeAvg()
{
    return std::make_unique<Sum>(Sum::Mode::Avg);
}
std::unique_ptr<AggregateState> makeMin()
{
    return std::make_unique<MinMax>(false);
}
std::unique_ptr<AggregateState> makeMax()
{
    return std::make_unique<MinMax>(true);
}
std::unique_ptr<AggregateState> makeGroupConcat()
{
    return std::make_unique<GroupConcat>(false);
}
std::unique_ptr<AggregateState> makeStringAgg()
{
    return std::make_unique<GroupConcat>(true);
}

void add(FunctionRegistry& r, const char* name, int minArgs, int maxArgs, AggregateFactory f,
         unsigned flags)
{
    FunctionDef d;
    d.name = name;
    d.minArgs = minArgs;
    d.maxArgs = maxArgs;
    d.flags = flags;
    d.aggregate = f;
    r.add(std::move(d));
}

}  // namespace

void registerAggregateFunctions(FunctionRegistry& r)
{
    const unsigned inv = kDeterministic | kInvertible;
    add(r, "count", 0, 1, makeCount, inv);
    add(r, "sum", 1, 1, makeSum, inv);
    add(r, "total", 1, 1, makeTotal, inv);
    add(r, "avg", 1, 1, makeAvg, inv);
    add(r, "min", 1, 1, makeMin, kDeterministic | kNeedsCollation);
    add(r, "max", 1, 1, makeMax, kDeterministic | kNeedsCollation);
    add(r, "group_concat", 1, 2, makeGroupConcat, inv);
    add(r, "string_agg", 2, 2, makeStringAgg, inv);
}

}  // namespace sql::internal
