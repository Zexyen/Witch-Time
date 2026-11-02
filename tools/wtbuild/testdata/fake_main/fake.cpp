// A stand-in "main" for wtbuild's lift test: no game code. Built for AArch64 by `wtbuild fake-nso` (src/fake.rs),
// into the committed main.nso and sdk.nso that the golden tests lift.
extern "C" {
void ext_log(int);
int ext_value(void);
void *ext_alloc(unsigned long);
float ext_scale(float);
}

struct Base {
    virtual ~Base() {}
    virtual int run(int x) { return x + 1; }
};
struct Derived : Base {
    int k = 3;
    int run(int x) override { return x * k; }
};

static int table_switch(int x)
{
    switch (x) {
    case 0: return ext_value() + 7;
    case 1: ext_log(1); return 11;
    case 2: return 13 * x;
    case 3: ext_log(3); return ext_value() - 5;
    case 4: return 17;
    case 5: ext_log(5); return 19;
    case 6: return 23 + ext_value();
    case 7: return 29;
    case 8: ext_log(8); return 31;
    default: return -1;
    }
}

extern "C" int caught(int x)
{
    try {
        if (x > 3)
            throw x;
        ext_log(x);
    } catch (int e) {
        return e * 2;
    }
    return 0;
}

extern "C" int cleanup(int x)
{
    Derived d;
    Base *b = &d;
    ext_log(b->run(x));
    return b->run(x + 1);
}

extern "C" void vec_math(float *a, const float *b, int n)
{
    for (int i = 0; i < n; i++)
        a[i] = a[i] * b[i] + ext_scale(b[i]) / 3.0f;
}

extern "C" double mixed(double a, long b, unsigned c)
{
    double r = a * b;
    if (r > 1e9)
        r = (double)(c >> 3) - a;
    return r + (float)c * 0.25 + __builtin_sqrt(a > 0 ? a : -a);
}

extern "C" long bits_and_shifts(unsigned long x, unsigned y)
{
    unsigned long r = (x >> (y & 31)) | (x << 7);
    r ^= __builtin_bswap64(x) + __builtin_clzl(x | 1) + __builtin_popcountl(x);
    return (long)r * (int)y / ((y & 7) + 1);
}

typedef int (*fnp)(int);
static fnp const callbacks[] = {table_switch, caught, cleanup};

extern "C" int dispatch(int i, int x)
{
    return callbacks[i % 3](x) + table_switch(x & 7);
}

extern "C" int nnMain(void)
{
    float a[16], b[16];
    for (int i = 0; i < 16; i++)
        a[i] = b[i] = (float)i;
    vec_math(a, b, 16);
    return dispatch(ext_value(), 2) + (int)mixed(a[3], 5, 9) + (int)bits_and_shifts(77, 4);
}

struct Guard {
    int v;
    ~Guard() { ext_log(v); }
};

extern "C" int nested(int x)
{
    Guard g{x};
    try {
        try {
            if (x & 1)
                throw 1.5;
            ext_log(x);
        } catch (double) {
            ext_log(-1);
            throw;
        }
    } catch (...) {
        return -x;
    }
    return x;
}

extern "C" int no_throw(int x) noexcept
{
    Guard g{x};
    return ext_value() + x;                 // a throw out of here ends in std::terminate
}

extern "C" int rethrow(int x)
{
    Guard a{1}, b{2};
    if (x > 9)
        throw x;
    return no_throw(x) + nested(x);
}
