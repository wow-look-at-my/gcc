// train-templates.cpp -- template-instantiation-heavy translation unit used
// as PGO training input for cc1plus (see README.md).  A trimmed version of
// the "big TU" benchmark: <regex> (deep libstdc++ template machinery),
// multi-container churn instantiated with several type combinations, and a
// recursive class-template instantiation storm that drives the
// tsubst/coercion/hashing paths that dominate cc1plus profiles on real
// template-heavy code.  Self-contained: standard headers only.
#include <regex>
#include <string>
#include <map>
#include <set>
#include <deque>
#include <list>
#include <vector>
#include <tuple>
#include <array>
#include <memory>
#include <algorithm>
#include <functional>

namespace churn {

template <typename T> struct Wrap {
    T v;
    T get() const { return v; }
};

template <typename K, typename V> double run() {
    std::map<K, V> m;
    std::set<K> s;
    std::deque<V> d;
    std::list<V> l;
    for (int i = 0; i < 16; ++i) {
        m[K(i)] = V(i);
        s.insert(K(i * 2));
        d.push_back(V(i));
        l.push_back(V(i));
    }
    double acc = 0;
    for (auto &kv : m)
        acc += double(kv.second);
    return acc + double(s.size() + d.size() + l.size());
}

double drive() {
    std::regex re("^[a-z]+([0-9]{2,4})$", std::regex::extended);
    std::smatch sm;
    std::string probe = "abc1234";
    double r = std::regex_match(probe, sm, re) ? 1.0 : 0.0;
    r += run<int, double>() + run<long, float>() + run<unsigned, long long>()
         + run<short, int>() + run<long long, double>();
    Wrap<std::string> w{probe};
    r += double(w.get().size());
    return r;
}

} // namespace churn

// Recursive instantiation storm: each H<N, Tag> instantiates distinct
// std::vector/std::map/std::tuple/std::array specializations (the array
// extents vary with N and Tag), plus H<N-1, Tag>.  DEPTH x TAGS distinct
// class template specializations keep the frontend's specialization tables
// and template-argument coercion hot -- exactly the paths PGO should see.
namespace storm {

template <int N, int Tag> struct H {
    std::vector<std::array<char, (N % 61) + 1>> v;
    std::map<std::array<char, (Tag % 13) + 1>, std::array<int, (N % 5) + 1>> mp;
    std::tuple<std::array<long, (N % 7) + 1>, std::array<short, (Tag % 11) + 1>> t;
    H<N - 1, Tag> next;
    long f() const { return N + long(v.size()) + long(mp.size()) + next.f(); }
};

template <int Tag> struct H<0, Tag> {
    long f() const { return Tag; }
};

template <int Tag> long one() {
    H<60, Tag> h{};
    return h.f();
}

long run_all() {
    long acc = 0;
    acc += one<1>();
    acc += one<2>();
    acc += one<3>();
    acc += one<4>();
    acc += one<5>();
    acc += one<6>();
    acc += one<7>();
    acc += one<8>();
    return acc;
}

} // namespace storm

int main() {
    return (int(churn::drive()) + int(storm::run_all() & 1)) & 0;
}
