/* Testes do recompilador: chama funções recompiladas do main.dol que não dependem de
 * hardware e compara com resultados calculados de forma independente no host. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ppc.h"
#include "ppc_funcs.h"

void ppc_mem_init(void);
int ppc_load_dol(const char *path);
PPCFunc ppc_lookup(u32 addr);

static PPCContext ctx;
static int g_fail, g_pass;

#define BUF_A 0x81000000u
#define BUF_B 0x81010000u
#define BUF_C 0x81020000u

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) g_pass++;                               \
        else { g_fail++; printf("  FALHA: " __VA_ARGS__); printf("\n"); } \
    } while (0)

static void call(u32 addr) {
    PPCFunc fn = ppc_lookup(addr);
    if (!fn) { printf("função 0x%08X não gerada\n", addr); exit(2); }
    ctx.lr = 0;
    fn(&ctx);
}

static u32 rng_state = 12345;
static u32 rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static void put_str(u32 ea, const char *s) { memcpy(host_ptr(ea), s, strlen(s) + 1); }
static void put_f32(u32 ea, float v) { st32(ea, f2u(v)); }
static float get_f32(u32 ea) { return u2f(ld32(ea)); }

/* endereços (de config/GM5E7D/symbols.txt) */
enum {
    A_init_registers = 0x800032B0, A_memset = 0x8000540C, A_memcpy = 0x800054F4,
    A_memcmp = 0x803910A0, A_memmove = 0x80391144, A_sprintf = 0x803914F0,
    A_strchr = 0x80393988, A_strncmp = 0x803939B8, A_strcmp = 0x803939F8,
    A_strncpy = 0x80393B20, A_strcpy = 0x80393B64, A_strlen = 0x80393C1C,
    A_sin = 0x803994B4, A_cvt_fp2unsigned = 0x8039A00C, A_div2u = 0x8039A198,
    A_div2i = 0x8039A284, A_mod2u = 0x8039A3BC, A_shl2i = 0x8039A4A0, A_shr2i = 0x8039A4E8,
    A_PSMTXIdentity = 0x80339EC0, A_PSMTXConcat = 0x80339EEC, A_PSMTXInverse = 0x80339FB8,
    A_PSMTXScale = 0x8033A0B0,
};

static void test_mem_string(void) {
    printf("memória e strings\n");
    for (int it = 0; it < 200; it++) {
        u32 n = rnd() % 300, off = rnd() % 8, val = rnd() & 0xFF;
        for (u32 i = 0; i < 400; i++) *host_ptr(BUF_A + i) = (u8)rnd();
        u8 ref[400];
        memcpy(ref, host_ptr(BUF_A), 400);
        memset(ref + off, (int)val, n);
        ctx.r[3] = BUF_A + off; ctx.r[4] = val; ctx.r[5] = n;
        call(A_memset);
        CHECK(memcmp(ref, host_ptr(BUF_A), 400) == 0, "memset n=%u off=%u", n, off);

        for (u32 i = 0; i < 400; i++) *host_ptr(BUF_B + i) = (u8)rnd();
        memcpy(ref, host_ptr(BUF_A), 400);
        u32 soff = rnd() % 8;
        memcpy(ref + off, host_ptr(BUF_B + soff), n);
        ctx.r[3] = BUF_A + off; ctx.r[4] = BUF_B + soff; ctx.r[5] = n;
        call(A_memcpy);
        CHECK(memcmp(ref, host_ptr(BUF_A), 400) == 0 && ctx.r[3] == BUF_A + off, "memcpy n=%u", n);

        /* memmove com sobreposição nos dois sentidos */
        u32 d = rnd() % 64, s = rnd() % 64;
        memcpy(ref, host_ptr(BUF_A), 400);
        memmove(ref + d, ref + s, n);
        ctx.r[3] = BUF_A + d; ctx.r[4] = BUF_A + s; ctx.r[5] = n;
        call(A_memmove);
        CHECK(memcmp(ref, host_ptr(BUF_A), 400) == 0, "memmove d=%u s=%u n=%u", d, s, n);

        memcpy(host_ptr(BUF_B), host_ptr(BUF_A), 400);
        if (n && (rnd() & 1)) *host_ptr(BUF_B + rnd() % n) ^= 0x5A;
        int want = memcmp(host_ptr(BUF_A), host_ptr(BUF_B), n);
        ctx.r[3] = BUF_A; ctx.r[4] = BUF_B; ctx.r[5] = n;
        call(A_memcmp);
        int got = (s32)ctx.r[3];
        CHECK((want < 0) == (got < 0) && (want > 0) == (got > 0), "memcmp n=%u want=%d got=%d", n, want, got);
    }

    const char *words[] = {"", "a", "metal", "arms", "glitch in the system", "Metal", "metal arms", "mettle"};
    int nw = sizeof words / sizeof *words;
    for (int i = 0; i < nw; i++) {
        put_str(BUF_A + 3, words[i]);
        ctx.r[3] = BUF_A + 3;
        call(A_strlen);
        CHECK(ctx.r[3] == strlen(words[i]), "strlen(\"%s\") = %u", words[i], ctx.r[3]);

        memset(host_ptr(BUF_B), 0xCC, 64);
        ctx.r[3] = BUF_B + 1; ctx.r[4] = BUF_A + 3;
        call(A_strcpy);
        CHECK(strcmp((char *)host_ptr(BUF_B + 1), words[i]) == 0, "strcpy \"%s\"", words[i]);

        memset(host_ptr(BUF_B), 0xCC, 64);
        char ref[64];
        memset(ref, 0xCC, 64);
        strncpy(ref, words[i], 6);
        ctx.r[3] = BUF_B; ctx.r[4] = BUF_A + 3; ctx.r[5] = 6;
        call(A_strncpy);
        CHECK(memcmp(ref, host_ptr(BUF_B), 64) == 0, "strncpy \"%s\"", words[i]);

        ctx.r[3] = BUF_A + 3; ctx.r[4] = 't';
        call(A_strchr);
        const char *p = strchr(words[i], 't');
        CHECK(ctx.r[3] == (p ? BUF_A + 3 + (u32)(p - words[i]) : 0), "strchr \"%s\"", words[i]);

        for (int j = 0; j < nw; j++) {
            put_str(BUF_A + 3, words[i]);
            put_str(BUF_B + 1, words[j]);
            int want = strcmp(words[i], words[j]);
            ctx.r[3] = BUF_A + 3; ctx.r[4] = BUF_B + 1;
            call(A_strcmp);
            int got = (s32)ctx.r[3];
            CHECK((want < 0) == (got < 0) && (want > 0) == (got > 0), "strcmp(\"%s\",\"%s\")", words[i], words[j]);
            want = strncmp(words[i], words[j], 4);
            ctx.r[3] = BUF_A + 3; ctx.r[4] = BUF_B + 1; ctx.r[5] = 4;
            call(A_strncmp);
            got = (s32)ctx.r[3];
            CHECK((want < 0) == (got < 0) && (want > 0) == (got > 0), "strncmp(\"%s\",\"%s\",4)", words[i], words[j]);
        }
    }
}

static u64 rnd64(void) { return ((u64)rnd() << 40) ^ ((u64)rnd() << 20) ^ rnd(); }

static void test_int64(void) {
    printf("aritmética de 64 bits do runtime CW\n");
    for (int it = 0; it < 2000; it++) {
        u64 a = rnd64(), b = rnd64() >> (rnd() % 60);
        if (!b) b = 1;
        if (it & 1) a >>= rnd() % 63;
        ctx.r[3] = (u32)(a >> 32); ctx.r[4] = (u32)a; ctx.r[5] = (u32)(b >> 32); ctx.r[6] = (u32)b;
        call(A_div2u);
        u64 q = ((u64)ctx.r[3] << 32) | ctx.r[4];
        CHECK(q == a / b, "__div2u %llx / %llx = %llx", a, b, q);

        ctx.r[3] = (u32)(a >> 32); ctx.r[4] = (u32)a; ctx.r[5] = (u32)(b >> 32); ctx.r[6] = (u32)b;
        call(A_mod2u);
        q = ((u64)ctx.r[3] << 32) | ctx.r[4];
        CHECK(q == a % b, "__mod2u %llx %% %llx = %llx", a, b, q);

        s64 sa = (s64)a * ((rnd() & 1) ? -1 : 1), sb = (s64)b * ((rnd() & 1) ? -1 : 1);
        ctx.r[3] = (u32)((u64)sa >> 32); ctx.r[4] = (u32)sa; ctx.r[5] = (u32)((u64)sb >> 32); ctx.r[6] = (u32)sb;
        call(A_div2i);
        s64 sq = (s64)(((u64)ctx.r[3] << 32) | ctx.r[4]);
        CHECK(sq == sa / sb, "__div2i %lld / %lld = %lld", sa, sb, sq);

        u32 sh = rnd() % 64;
        ctx.r[3] = (u32)(a >> 32); ctx.r[4] = (u32)a; ctx.r[5] = sh;
        call(A_shl2i);
        q = ((u64)ctx.r[3] << 32) | ctx.r[4];
        CHECK(q == (a << sh), "__shl2i %llx << %u", a, sh);

        ctx.r[3] = (u32)((u64)sa >> 32); ctx.r[4] = (u32)sa; ctx.r[5] = sh;
        call(A_shr2i);
        sq = (s64)(((u64)ctx.r[3] << 32) | ctx.r[4]);
        CHECK(sq == (sa >> sh), "__shr2i %lld >> %u", sa, sh);
    }
}

static void test_float(void) {
    printf("ponto flutuante (MSL)\n");
    double worst = 0;
    for (int it = 0; it < 5000; it++) {
        double x = ((double)rnd() / (1 << 24) - 0.5) * 200.0;
        ctx.f[1].ps0 = x;
        call(A_sin);
        double want = sin(x), got = ctx.f[1].ps0;
        double err = fabs(got - want) / fmax(fabs(want), 1e-300);
        if (err > worst) worst = err;
        CHECK(err < 1e-15 || fabs(got - want) < 1e-300, "sin(%.17g) = %.17g, host %.17g", x, got, want);
    }
    printf("  maior erro relativo do sin: %.3g\n", worst);

    double vals[] = {0.0, 0.5, 1.0, 1.99, 255.7, 65535.9, 2147483647.0, 2147483648.0, 3e9, 4294967295.0};
    for (int i = 0; i < (int)(sizeof vals / sizeof *vals); i++) {
        ctx.f[1].ps0 = vals[i];
        call(A_cvt_fp2unsigned);
        CHECK(ctx.r[3] == (u32)vals[i], "__cvt_fp2unsigned(%.17g) = %u", vals[i], ctx.r[3]);
    }
}

static void sprintf_case(const char *fmt, int nint, const u32 *ints, int ndbl, const double *dbls,
                         const char *want) {
    put_str(BUF_B, fmt);
    memset(host_ptr(BUF_A), 0, 256);
    ctx.r[3] = BUF_A;
    ctx.r[4] = BUF_B;
    for (int i = 0; i < nint; i++) ctx.r[5 + i] = ints[i];
    for (int i = 0; i < ndbl; i++) ctx.f[1 + i].ps0 = dbls[i];
    set_crbit(&ctx, 6, ndbl > 0); /* EABI: cr1[eq] avisa que há FP em registradores */
    call(A_sprintf);
    const char *got = (const char *)host_ptr(BUF_A);
    CHECK(strcmp(got, want) == 0 && ctx.r[3] == strlen(want), "sprintf(\"%s\") = \"%s\" (esperado \"%s\")", fmt, got, want);
}

static void test_sprintf(void) {
    printf("sprintf (MSL, passa por __pformatter e callbacks indiretos)\n");
    char want[256];
    u32 ints[4];
    double d[2];

    ints[0] = 42; ints[1] = (u32)-7; ints[2] = 0xBEEF;
    snprintf(want, sizeof want, "%d %d %x", 42, -7, 0xBEEF);
    sprintf_case("%d %d %x", 3, ints, 0, d, want);

    put_str(BUF_C, "Glitch");
    ints[0] = BUF_C; ints[1] = 'Z'; ints[2] = 123;
    snprintf(want, sizeof want, "[%s] %c |%-6d| %08X", "Glitch", 'Z', 123, 0x1234ABCDu);
    ints[3] = 0x1234ABCD;
    sprintf_case("[%s] %c |%-6d| %08X", 4, ints, 0, d, want);

    d[0] = 3.14159265358979; d[1] = -0.001234;
    snprintf(want, sizeof want, "%.3f %10.5f", d[0], d[1]);
    sprintf_case("%.3f %10.5f", 0, ints, 2, d, want);

    d[0] = 1234567.875;
    ints[0] = 99;
    snprintf(want, sizeof want, "pts=%d t=%.2f", 99, 1234567.875);
    sprintf_case("pts=%d t=%.2f", 1, ints, 1, d, want);
}

static void put_mtx(u32 ea, const double m[3][4]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) put_f32(ea + (u32)(i * 16 + j * 4), (float)m[i][j]);
}

static void test_mtx(void) {
    printf("matrizes (paired singles)\n");
    ctx.gqr[0] = 0; /* quantização 'float', como o OSInit deixa */
    ctx.r[3] = BUF_A;
    call(A_PSMTXIdentity);
    int ok = 1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) ok &= get_f32(BUF_A + (u32)(i * 16 + j * 4)) == (i == j ? 1.0f : 0.0f);
    CHECK(ok, "PSMTXIdentity");

    for (int it = 0; it < 500; it++) {
        double a[3][4], b[3][4], ab[3][4];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                a[i][j] = (float)(((double)rnd() / (1 << 24) - 0.5) * 8);
                b[i][j] = (float)(((double)rnd() / (1 << 24) - 0.5) * 8);
            }
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                ab[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0);
        put_mtx(BUF_A, a);
        put_mtx(BUF_B, b);
        ctx.r[3] = BUF_A; ctx.r[4] = BUF_B; ctx.r[5] = BUF_C;
        call(A_PSMTXConcat);
        double worst = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) worst = fmax(worst, fabs(get_f32(BUF_C + (u32)(i * 16 + j * 4)) - ab[i][j]));
        CHECK(worst < 1e-4, "PSMTXConcat erro %.3g", worst);

        /* inversa: A * inv(A) deve dar a identidade */
        ctx.r[3] = BUF_A; ctx.r[4] = BUF_B;
        call(A_PSMTXInverse);
        if (ctx.r[3]) {
            ctx.r[3] = BUF_A; ctx.r[4] = BUF_B; ctx.r[5] = BUF_C;
            call(A_PSMTXConcat);
            double w = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++) w = fmax(w, fabs(get_f32(BUF_C + (u32)(i * 16 + j * 4)) - (i == j)));
            CHECK(w < 2e-2, "PSMTXInverse: A*inv(A) desvio %.3g", w);
        }
    }

    ctx.r[3] = BUF_A;
    ctx.f[1].ps0 = 2.0; ctx.f[2].ps0 = -3.0; ctx.f[3].ps0 = 0.5;
    call(A_PSMTXScale);
    float want[12] = {2, 0, 0, 0, 0, -3, 0, 0, 0, 0, 0.5f, 0};
    ok = 1;
    for (int i = 0; i < 12; i++) ok &= get_f32(BUF_A + (u32)i * 4) == want[i];
    CHECK(ok, "PSMTXScale");
}

int main(int argc, char **argv) {
    const char *dol = argc > 1 ? argv[1] : "orig/GM5E7D/sys/main.dol";
    ppc_mem_init();
    if (!ppc_load_dol(dol)) { printf("não abriu %s\n", dol); return 2; }
    call(A_init_registers);
    printf("__init_registers: r1=%08X r2=%08X r13=%08X\n", ctx.r[1], ctx.r[2], ctx.r[13]);
    CHECK(ctx.r[1] == 0x804D2AA0 && ctx.r[2] == 0x804C2420 && ctx.r[13] == 0x804BABE0, "__init_registers");

    test_mem_string();
    test_int64();
    test_float();
    test_sprintf();
    test_mtx();

    printf("\n%d ok, %d falhas\n", g_pass, g_fail);
    return g_fail != 0;
}
