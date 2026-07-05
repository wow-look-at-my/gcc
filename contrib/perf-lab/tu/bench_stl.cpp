// bench_stl.cpp — header-heavy realistic translation unit for compile-time
// benchmarking. Exercises common STL containers, algorithms, std::function
// callbacks, and several class templates instantiated with multiple types.
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <memory>
#include <functional>
#include <chrono>
#include <sstream>
#include <iostream>

namespace bench {

// ---------------------------------------------------------------------------
// Basic domain types
// ---------------------------------------------------------------------------

struct Point {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    double norm2() const { return x * x + y * y + z * z; }
    Point operator+(const Point& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Point operator-(const Point& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Point scaled(double s) const { return {x * s, y * s, z * s}; }
};

bool operator<(const Point& a, const Point& b) {
    return a.norm2() < b.norm2();
}

struct Record {
    int id = 0;
    std::string name;
    std::vector<double> samples;
    std::chrono::system_clock::time_point created;

    double mean() const {
        if (samples.empty()) return 0.0;
        double sum = 0.0;
        for (double s : samples) sum += s;
        return sum / static_cast<double>(samples.size());
    }
};

// ---------------------------------------------------------------------------
// Class template: a small fixed-capacity ring buffer, instantiated with
// several element types below.
// ---------------------------------------------------------------------------

template <typename T, std::size_t N>
class RingBuffer {
public:
    void push(const T& v) {
        data_[head_] = v;
        head_ = (head_ + 1) % N;
        if (size_ < N) ++size_;
    }

    template <typename... Args>
    void emplace(Args&&... args) {
        data_[head_] = T(std::forward<Args>(args)...);
        head_ = (head_ + 1) % N;
        if (size_ < N) ++size_;
    }

    std::size_t size() const { return size_; }

    const T& at(std::size_t i) const { return data_[(head_ + N - size_ + i) % N]; }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t i = 0; i < size_; ++i) fn(at(i));
    }

private:
    T data_[N] {};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// Class template: an event dispatcher keyed by string, holding
// std::function callbacks — instantiated with several payload types.
// ---------------------------------------------------------------------------

template <typename Payload>
class Dispatcher {
public:
    using Handler = std::function<void(const Payload&)>;

    void subscribe(const std::string& topic, Handler h) {
        handlers_[topic].push_back(std::move(h));
    }

    std::size_t publish(const std::string& topic, const Payload& p) const {
        auto it = handlers_.find(topic);
        if (it == handlers_.end()) return 0;
        for (const auto& h : it->second) h(p);
        return it->second.size();
    }

    std::size_t topics() const { return handlers_.size(); }

private:
    std::unordered_map<std::string, std::vector<Handler>> handlers_;
};

// ---------------------------------------------------------------------------
// Class template: a memoizing cache wrapping a std::function.
// ---------------------------------------------------------------------------

template <typename Key, typename Value>
class MemoCache {
public:
    explicit MemoCache(std::function<Value(const Key&)> compute)
        : compute_(std::move(compute)) {}

    const Value& get(const Key& k) {
        auto it = cache_.find(k);
        if (it != cache_.end()) {
            ++hits_;
            return it->second;
        }
        ++misses_;
        auto res = cache_.emplace(k, compute_(k));
        return res.first->second;
    }

    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }

private:
    std::function<Value(const Key&)> compute_;
    std::map<Key, Value> cache_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

// ---------------------------------------------------------------------------
// Statistics helpers using <algorithm> heavily.
// ---------------------------------------------------------------------------

template <typename Container>
double median_of(Container c) {
    if (c.empty()) return 0.0;
    std::sort(c.begin(), c.end());
    const std::size_t mid = c.size() / 2;
    if (c.size() % 2 == 0)
        return (c[mid - 1] + c[mid]) / 2.0;
    return c[mid];
}

std::vector<double> normalize(std::vector<double> v) {
    auto mm = std::minmax_element(v.begin(), v.end());
    if (mm.first == v.end() || *mm.first == *mm.second) return v;
    const double lo = *mm.first, hi = *mm.second;
    std::transform(v.begin(), v.end(), v.begin(),
                   [lo, hi](double d) { return (d - lo) / (hi - lo); });
    return v;
}

struct Summary {
    double mean = 0.0;
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
};

Summary summarize(const std::vector<double>& v) {
    Summary s;
    if (v.empty()) return s;
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    double sum = 0.0;
    for (double d : v) sum += d;
    s.mean = sum / static_cast<double>(v.size());
    s.median = median_of(v);
    return s;
}

// ---------------------------------------------------------------------------
// String/stream utilities.
// ---------------------------------------------------------------------------

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) oss << sep;
        oss << parts[i];
    }
    return oss.str();
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, delim)) out.push_back(tok);
    return out;
}

std::string describe(const Record& r) {
    std::ostringstream oss;
    oss << "Record{" << r.id << ", \"" << r.name << "\", n=" << r.samples.size()
        << ", mean=" << r.mean() << "}";
    return oss.str();
}

// ---------------------------------------------------------------------------
// A registry tying it together: maps, shared_ptr ownership, callbacks.
// ---------------------------------------------------------------------------

class Registry {
public:
    using Validator = std::function<bool(const Record&)>;

    void add_validator(Validator v) { validators_.push_back(std::move(v)); }

    bool insert(std::shared_ptr<Record> rec) {
        if (!rec) return false;
        for (const auto& v : validators_)
            if (!v(*rec)) return false;
        by_id_[rec->id] = rec;
        by_name_.emplace(rec->name, rec);
        return true;
    }

    std::shared_ptr<Record> find(int id) const {
        auto it = by_id_.find(id);
        return it == by_id_.end() ? nullptr : it->second;
    }

    std::vector<std::shared_ptr<Record>> top_by_mean(std::size_t k) const {
        std::vector<std::shared_ptr<Record>> all;
        all.reserve(by_id_.size());
        for (const auto& kv : by_id_) all.push_back(kv.second);
        std::partial_sort(all.begin(),
                          all.begin() + std::min(k, all.size()),
                          all.end(),
                          [](const auto& a, const auto& b) {
                              return a->mean() > b->mean();
                          });
        if (all.size() > k) all.resize(k);
        return all;
    }

    std::size_t size() const { return by_id_.size(); }

private:
    std::map<int, std::shared_ptr<Record>> by_id_;
    std::unordered_map<std::string, std::shared_ptr<Record>> by_name_;
    std::vector<Validator> validators_;
};

// ---------------------------------------------------------------------------
// Instantiate the class templates with several distinct types.
// ---------------------------------------------------------------------------

using IntRing = RingBuffer<int, 16>;
using PointRing = RingBuffer<Point, 8>;
using StringRing = RingBuffer<std::string, 4>;
using RecordRing = RingBuffer<Record, 4>;

using IntDispatcher = Dispatcher<int>;
using StringDispatcher = Dispatcher<std::string>;
using PointDispatcher = Dispatcher<Point>;
using RecordDispatcher = Dispatcher<Record>;

using FibCache = MemoCache<int, long long>;
using NameCache = MemoCache<std::string, std::size_t>;
using PointCache = MemoCache<int, Point>;

long long fib_impl(int n, FibCache& c);

FibCache make_fib_cache() {
    static FibCache* self = nullptr;
    FibCache c([](const int& n) -> long long {
        if (n < 2) return n;
        return fib_impl(n - 1, *self) + fib_impl(n - 2, *self);
    });
    self = &c;
    return c;
}

long long fib_impl(int n, FibCache& c) { return c.get(n); }

int run_all() {
    IntRing ir;
    for (int i = 0; i < 40; ++i) ir.push(i * i);
    PointRing pr;
    for (int i = 0; i < 12; ++i) pr.emplace();
    StringRing sr;
    sr.push("alpha");
    sr.push("beta");
    sr.push("gamma");
    RecordRing rr;
    rr.emplace();

    long long acc = 0;
    ir.for_each([&acc](int v) { acc += v; });

    IntDispatcher idisp;
    idisp.subscribe("tick", [&acc](const int& v) { acc += v; });
    idisp.subscribe("tick", [&acc](const int& v) { acc -= v / 2; });
    idisp.publish("tick", 7);

    StringDispatcher sdisp;
    std::vector<std::string> seen;
    sdisp.subscribe("log", [&seen](const std::string& s) { seen.push_back(s); });
    sdisp.publish("log", "hello");
    sdisp.publish("log", "world");

    PointDispatcher pdisp;
    Point sum;
    pdisp.subscribe("move", [&sum](const Point& p) { sum = sum + p; });
    pdisp.publish("move", Point{1, 2, 3});

    RecordDispatcher rdisp;
    rdisp.subscribe("new", [](const Record& r) { (void)r.mean(); });

    NameCache nc([](const std::string& s) { return s.size() * 31u; });
    (void)nc.get("abc");
    (void)nc.get("abc");
    (void)nc.get("defgh");

    PointCache pc([](const int& i) {
        return Point{static_cast<double>(i), static_cast<double>(i * 2),
                     static_cast<double>(i * 3)};
    });
    (void)pc.get(4);

    Registry reg;
    reg.add_validator([](const Record& r) { return !r.name.empty(); });
    reg.add_validator([](const Record& r) { return r.id >= 0; });
    for (int i = 0; i < 20; ++i) {
        auto rec = std::make_shared<Record>();
        rec->id = i;
        rec->name = "record-" + std::to_string(i);
        for (int j = 0; j <= i; ++j)
            rec->samples.push_back(static_cast<double>(j * i) / 7.0);
        rec->created = std::chrono::system_clock::now();
        reg.insert(rec);
    }
    auto top = reg.top_by_mean(5);

    std::vector<double> vals;
    for (int i = 0; i < 100; ++i)
        vals.push_back(static_cast<double>((i * 37) % 101));
    auto norm = normalize(vals);
    Summary s = summarize(norm);

    std::vector<std::string> parts = split("a,b,c,d,e", ',');
    std::string joined = join(parts, "|");

    std::ostringstream report;
    report << "acc=" << acc << " topics=" << idisp.topics()
           << " top=" << top.size() << " median=" << s.median
           << " joined=" << joined << " sum.norm2=" << sum.norm2();
    return static_cast<int>(report.str().size());
}

} // namespace bench

int main() {
    int r = bench::run_all();
    std::cout << "bench_stl ok: " << r << "\n";
    return r > 0 ? 0 : 1;
}
