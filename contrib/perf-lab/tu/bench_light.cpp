// bench_light.cpp — minimal translation unit to expose fixed per-invocation
// compiler startup overhead. Only <cstdio>, plain C-ish code.
#include <cstdio>

static int accumulate(const int* data, int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += data[i];
    return sum;
}

static int fib(int n) {
    if (n < 2) return n;
    int a = 0, b = 1;
    for (int i = 2; i <= n; ++i) {
        int t = a + b;
        a = b;
        b = t;
    }
    return b;
}

static unsigned crc_ish(const char* s) {
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

struct Pair {
    int key;
    int value;
};

static int lookup(const Pair* table, int n, int key) {
    for (int i = 0; i < n; ++i)
        if (table[i].key == key) return table[i].value;
    return -1;
}

int main(void) {
    int data[16];
    for (int i = 0; i < 16; ++i) data[i] = i * 3 - 5;
    int sum = accumulate(data, 16);

    Pair table[4] = {{1, 10}, {2, 20}, {3, 30}, {4, 40}};
    int v = lookup(table, 4, 3);

    unsigned h = crc_ish("bench_light");
    int f = fib(20);

    std::printf("sum=%d v=%d h=%u fib=%d\n", sum, v, h, f);
    return (sum + v + f) != 0 ? 0 : 1;
}
