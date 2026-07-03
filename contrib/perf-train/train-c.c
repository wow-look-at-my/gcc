/* train-c.c -- small C translation unit used as PGO training input for cc1
   (see README.md).  Plain C so the C front end also records profile data:
   structs, loops, switches, function pointers, string handling and varargs
   formatting -- ordinary C shapes.  Self-contained: libc headers only.  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct node {
    int key;
    double weight;
    struct node *next;
};

static struct node *make_chain(int n)
{
    struct node *head = NULL;
    for (int i = 0; i < n; ++i) {
        struct node *nn = malloc(sizeof *nn);
        if (!nn)
            abort();
        nn->key = i;
        nn->weight = (double)(i * 31 % 17) / 4.0;
        nn->next = head;
        head = nn;
    }
    return head;
}

static double fold(const struct node *p, double (*op)(double, double))
{
    double acc = 0.0;
    for (; p; p = p->next)
        acc = op(acc, p->weight);
    return acc;
}

static double add(double a, double b) { return a + b; }
static double mix(double a, double b) { return a * 0.5 + b; }

static const char *classify(int k)
{
    switch (k & 7) {
    case 0: return "zero";
    case 1: case 2: return "low";
    case 3: case 4: case 5: return "mid";
    default: return "high";
    }
}

static void free_chain(struct node *p)
{
    while (p) {
        struct node *nx = p->next;
        free(p);
        p = nx;
    }
}

int main(void)
{
    struct node *c = make_chain(256);
    double a = fold(c, add), m = fold(c, mix);
    char buf[128];
    int n = snprintf(buf, sizeof buf, "%s/%s: %.3f %.3f",
                     classify(11), classify(4), a, m);
    if (n <= 0 || (size_t)n >= sizeof buf)
        return 1;
    int cmp = strncmp(buf, "mid/", 4);
    free_chain(c);
    return cmp == 0 ? 0 : 1;
}
