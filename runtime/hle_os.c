/* HLE de OS, alarmes, controle e memory card. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>

#include "rt.h"

/* ---- OSReport / OSPanic ----------------------------------------------------- */

/* Formata com o sprintf do próprio jogo (MSL). fmt_reg é o registrador do formato;
 * os argumentos seguintes são deslocados para depois do buffer. */
static const char *guest_format(PPCContext *c, int fmt_reg) {
    PPCContext tmp = *c;
    u32 buf = (c->r[1] - 0x1000) & ~0xFu;
    tmp.r[1] = buf - 0x100;
    st32(tmp.r[1], 0);
    tmp.r[3] = buf;
    for (int i = 0; fmt_reg + i <= 10 && 4 + i <= 10; i++) tmp.r[4 + i] = c->r[fmt_reg + i];
    tmp.lr = 0;
    ppc_lookup(SYM_sprintf)(&tmp);
    return (const char *)host_ptr(buf);
}

void hle_OSReport(PPCContext *c) {
    const char *s = guest_format(c, 3);
    size_t n = strlen(s);
    fprintf(stdout, "[OSReport] %.*s\n", (int)(n && s[n - 1] == '\n' ? n - 1 : n), s);
    fflush(stdout);
}

void hle_OSPanic(PPCContext *c) {
    const char *file = (const char *)host_ptr(ARG(0));
    u32 line = ARG(1);
    const char *msg = guest_format(c, 5);
    rt_fatal("OSPanic em %s:%u: %s", file, line, msg);
}

void hle_OSResetSystem(PPCContext *c) {
    LOG("OSResetSystem(%u, %08X, %u): encerrando", ARG(0), ARG(1), ARG(2));
    fflush(stdout);
    exit(0);
}

/* ---- vídeos Bink ------------------------------------------------------------ */

/* BinkOpen(nome, flags). Com MA_SKIP_MOVIES=1 o vídeo abre normalmente, mas o total
 * de quadros (HBINK + 8) vira 1, e o player do jogo termina logo. Falhar a abertura
 * não serve: o jogo trata como disco defeituoso. (Atalho de desenvolvimento.) */
/* Level_FindIndex(título): procura a fase na tabela 0x803C93C0 (57 entradas de 44 bytes:
 * 0 = teste "Generic", 1-42 = campanha, 43-56 = multijogador) e devolve o índice.
 * Desenvolvimento: MA_LEVEL=<n> troca a primeira fase carregada pelo carregador de fases
 * (fn_801B0AE8, retorno em 0x801B0B48) pela entrada n, para testar fases sem jogar até elas. */
void hle_Level_FindIndex(PPCContext *c) {
    static int forced = -2;
    if (forced == -2) forced = getenv("MA_LEVEL") ? atoi(getenv("MA_LEVEL")) : -1;
    u32 lr = c->lr, title = ARG(0);
    f_801B0A74_orig(c);
    if (lr != 0x801B0B48) return;
    LOG("[fase] carregando \"%s\" (entrada %d)", (const char *)host_ptr(title), (s32)c->r[3]);
    if (forced >= 0 && (s32)c->r[3] >= 1 && forced < 57) {
        u32 name = ld32(0x803C93C0 + 44 * (u32)forced);
        LOG("[fase] MA_LEVEL: trocando por \"%s\" (entrada %d)", name ? (const char *)host_ptr(name) : "?", forced);
        c->r[3] = (u32)forced;
        forced = -1;
    }
}

void hle_BinkOpen(PPCContext *c) {
    static int skip = -1;
    if (skip < 0) skip = getenv("MA_SKIP_MOVIES") != NULL;
    VLOG("[bink] abrir \"%s\"%s", (const char *)host_ptr(ARG(0)), skip ? " (encurtado)" : "");
    f_80368A50_orig(c);
    if (skip && c->r[3]) st32(c->r[3] + 8, 1);
}

/* ---- alarmes -------------------------------------------------------------- */

/* OSAlarm: handler 0x00, tag 0x04, fire 0x08, prev 0x10, next 0x14, period 0x18, start 0x20 */

static void alarm_fire(PPCContext *c, u64 a64) {
    u32 a = (u32)a64;
    u32 handler = ld32(a);
    if (!handler) return;
    u64 period = ld64(a + 0x18);
    if (period) {
        u64 next = ld64(a + 0x08) + period;
        st64(a + 0x08, next);
        ev_schedule(next, alarm_fire, a);
    } else {
        st32(a, 0);
    }
    guest_callback(c, handler, a, ld32(0x800000D4), 0);
}

void hle_OSCreateAlarm(PPCContext *c) {
    st32(ARG(0), 0);
    st32(ARG(0) + 4, 0);
}

void hle_OSSetAlarm(PPCContext *c) {
    u32 a = ARG(0);
    u64 tick = ((u64)c->r[5] << 32) | c->r[6];
    u32 handler = c->r[7];
    ev_cancel(alarm_fire, a);
    u64 fire = tb_now() + tick;
    st32(a, handler);
    st64(a + 0x08, fire);
    st64(a + 0x18, 0);
    ev_schedule(fire, alarm_fire, a);
}

void hle_OSSetPeriodicAlarm(PPCContext *c) {
    u32 a = ARG(0);
    u64 start = ((u64)c->r[5] << 32) | c->r[6];
    u64 period = ((u64)c->r[7] << 32) | c->r[8];
    u32 handler = c->r[9];
    ev_cancel(alarm_fire, a);
    u64 now = tb_now(), fire = start;
    if (period && fire < now) fire += ((now - fire) / period + 1) * period;
    st32(a, handler);
    st64(a + 0x08, fire);
    st64(a + 0x18, period);
    st64(a + 0x20, start);
    ev_schedule(fire, alarm_fire, a);
}

void hle_OSCancelAlarm(PPCContext *c) {
    ev_cancel(alarm_fire, ARG(0));
    st32(ARG(0), 0);
}

/* ---- controle (PAD) ------------------------------------------------------- */

enum {
    PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010,
    PAD_R = 0x0020, PAD_L = 0x0040, PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400,
    PAD_Y = 0x0800, PAD_START = 0x1000
};

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* Teclado configurável: tecla_<botão>=<tecla> no metalarms.ini vira MA_TECLA_<BOTÃO>. */
enum {
    K_A, K_B, K_X, K_Y, K_Z, K_L, K_R, K_START, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_CUP, K_CDOWN, K_CLEFT, K_CRIGHT, K_DUP, K_DDOWN, K_DLEFT, K_DRIGHT, K_COUNT
};
#define KEYS_PER_BUTTON 4
static struct { const char *name; int vk[KEYS_PER_BUTTON]; } g_keymap[K_COUNT] = {
    {"A", {VK_SPACE}}, {"B", {VK_TAB}}, {"X", {'F'}}, {"Y", {'V'}}, {"Z", {VK_RBUTTON}}, {"L", {'Z'}},
    {"R", {VK_LBUTTON, 'E'}}, {"START", {VK_ESCAPE, VK_RETURN}},
    {"CIMA", {'W'}}, {"BAIXO", {'S'}}, {"ESQUERDA", {'A'}}, {"DIREITA", {'D'}},
    {"C_CIMA", {'I'}}, {"C_BAIXO", {'K'}}, {"C_ESQUERDA", {'J'}}, {"C_DIREITA", {'L'}},
    {"DIR_CIMA", {VK_UP}}, {"DIR_BAIXO", {VK_DOWN}}, {"DIR_ESQUERDA", {VK_LEFT}}, {"DIR_DIREITA", {VK_RIGHT}},
};

/* nome de tecla -> virtual-key: letra, dígito, F1-F24, NUM0-9 ou um dos nomes da tabela */
static int parse_key(const char *s) {
    static const struct { const char *name; int vk; } names[] = {
        {"ENTER", VK_RETURN}, {"ESPACO", VK_SPACE}, {"TAB", VK_TAB}, {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL},
        {"ALT", VK_MENU}, {"BACKSPACE", VK_BACK}, {"SETA_CIMA", VK_UP}, {"SETA_BAIXO", VK_DOWN},
        {"SETA_ESQUERDA", VK_LEFT}, {"SETA_DIREITA", VK_RIGHT}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"SHIFT_ESQ", VK_LSHIFT}, {"SHIFT_DIR", VK_RSHIFT}, {"CTRL_ESQ", VK_LCONTROL}, {"CTRL_DIR", VK_RCONTROL},
        {"MOUSE_MEIO", VK_MBUTTON}, {"MOUSE_4", VK_XBUTTON1}, {"MOUSE_5", VK_XBUTTON2}, {"ESC", VK_ESCAPE},
        {"MOUSE_ESQ", VK_LBUTTON}, {"MOUSE_DIR", VK_RBUTTON}, {"NENHUMA", -1},
    };
    if (!s[0]) return 0;
    if (!s[1]) {
        int ch = toupper((unsigned char)s[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return ch;
        return 0;
    }
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int n = atoi(s + 1);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
    }
    if (!_strnicmp(s, "NUM", 3) && isdigit((unsigned char)s[3]) && !s[4]) return VK_NUMPAD0 + (s[3] - '0');
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!_stricmp(s, names[i].name)) return names[i].vk;
    return 0;
}

static void keymap_load(void) {
    static int loaded;
    if (loaded) return;
    loaded = 1;
    for (int i = 0; i < K_COUNT; i++) {
        char env[48];
        snprintf(env, sizeof env, "MA_TECLA_%s", g_keymap[i].name);
        const char *v = getenv(env);
        if (!v) continue;
        /* várias teclas separadas por vírgula: "MOUSE_ESQ,E" */
        int vks[KEYS_PER_BUTTON] = {0}, n = 0, ok = 1;
        char buf[128];
        snprintf(buf, sizeof buf, "%s", v);
        for (char *tok = strtok(buf, ", "); tok && n < KEYS_PER_BUTTON; tok = strtok(NULL, ", ")) {
            int vk = parse_key(tok);
            if (!vk) ok = 0;
            else if (vk > 0) vks[n++] = vk;
        }
        if (ok) memcpy(g_keymap[i].vk, vks, sizeof vks);
        else LOG("[teclado] tecla desconhecida em tecla_%s=\"%s\"; ficou a padrão", g_keymap[i].name, v);
    }
}

/* estado dos botões do mouse nesta leitura (só valem com o mouse capturado) */
static int g_mouse_l, g_mouse_r, g_mouse_captured_now, g_focused_now;

static int vk_down(int vk) {
    if (vk == VK_LBUTTON) return g_mouse_l;
    if (vk == VK_RBUTTON) return g_mouse_r;
    if (vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) return g_mouse_captured_now && key(vk);
    return g_focused_now && key(vk);
}

static int kdown(int k) {
    for (int i = 0; i < KEYS_PER_BUTTON && g_keymap[k].vk[i]; i++)
        if (vk_down(g_keymap[k].vk[i])) return 1;
    return 0;
}

/* MA_INPUT="1200:A,1400:START,2000-2600:SU": aperta o botão por 6 retraces a partir
 * daquele retrace, ou durante o intervalo inicio-fim (testes automáticos sem teclado).
 * SU/SD/SL/SR empurram o analógico para cima/baixo/esquerda/direita. */
static s8 g_script_sx, g_script_sy, g_script_cx, g_script_cy;

static u16 scripted_buttons(void) {
    static const char *script;
    static int loaded;
    if (!loaded) { script = getenv("MA_INPUT"); loaded = 1; }
    g_script_sx = g_script_sy = g_script_cx = g_script_cy = 0;
    if (!script) return 0;
    static const struct { const char *name; u16 bit; } names[] = {
        {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"Z", PAD_Z}, {"L", PAD_L}, {"R", PAD_R},
        {"START", PAD_START}, {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT}};
    u16 b = 0;
    for (const char *p = script; *p;) {
        unsigned at = (unsigned)strtoul(p, (char **)&p, 10), end = at + 6;
        if (*p == '-') { p++; end = (unsigned)strtoul(p, (char **)&p, 10); }
        if (*p == ':') p++;
        const char *name = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - name);
        if (*p == ',') p++;
        if (g_retrace_count < at || g_retrace_count >= end) continue;
        if (len == 2 && name[0] == 'S') {
            if (name[1] == 'U') g_script_sy = 100;
            if (name[1] == 'D') g_script_sy = -100;
            if (name[1] == 'L') g_script_sx = -100;
            if (name[1] == 'R') g_script_sx = 100;
            continue;
        }
        if (len == 2 && name[0] == 'C') { /* C-stick: CU, CD, CL, CR */
            if (name[1] == 'U') g_script_cy = 100;
            if (name[1] == 'D') g_script_cy = -100;
            if (name[1] == 'L') g_script_cx = -100;
            if (name[1] == 'R') g_script_cx = 100;
            continue;
        }
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == len && !strncmp(names[i].name, name, len)) b |= names[i].bit;
    }
    return b;
}

/* ---- controle XInput (carregado dinamicamente) -------------------------------- */

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XGamepad;
typedef struct { DWORD dwPacketNumber; XGamepad Gamepad; } XState;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVibration;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XState *);
typedef DWORD(WINAPI *XInputSetStateFn)(DWORD, XVibration *);
static XInputGetStateFn g_xget;
static XInputSetStateFn g_xset;

static void xinput_load(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !g_xget; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (!m) continue;
        g_xget = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
        g_xset = (XInputSetStateFn)GetProcAddress(m, "XInputSetState");
    }
}

/* eixo XInput (-32768..32767) -> analógico do GameCube (~-100..100) com zona morta */
static s8 stick_axis(SHORT v, int deadzone) {
    int x = v;
    if (x > -deadzone && x < deadzone) return 0;
    int r = x * 100 / 32767;
    return (s8)(r > 127 ? 127 : r < -128 ? -128 : r);
}

int gx_mouse_poll(int *dx, int *dy, int *left, int *right);

static int g_aim_dx, g_aim_dy; /* movimento do mouse na última leitura do controle */

static float rd_f32(u32 a) { u32 w = ld32(a); float f; memcpy(&f, &w, 4); return f; }
static void wr_f32(u32 a, float f) { u32 w; memcpy(&w, &f, 4); st32(a, w); }
static float wrap_pi(float a) { return a - 6.2831853f * floorf((a + 3.14159265f) / 6.2831853f); }

/* Mira com o mouse em malha fechada.
 *
 * O jogo transforma o C-stick em giro com curvas próprias. Medido por quadro com MA_AIM_TEST e
 * MA_TRACE: na horizontal, ~0,0006 rad com v = 0,25, ~0,004 com 0,5, ~0,045 com 1, ~0,064 com 2,
 * ~0,082 com 3 e no máximo ~0,089 (~300 graus/s), com uma aceleração leve; na vertical,
 * ~0,0127 x v^2. O passo de tempo do jogo varia (±10% por quadro). Somar o mouse ao C-stick fazia
 * os movimentos lentos quase não girarem e os rápidos dispararem.
 *
 * Aqui o mouse vira um ângulo a percorrer (o mesmo por unidade nos dois eixos). A cada amostra,
 * o vetor global da mira em 0x80417308 diz quanto já girou: ele muda no mesmo quadro do comando,
 * ao contrário da câmera (0x804381C0), que vem um quadro depois e por isso fazia a mira passar
 * do ponto e tremer. A curva inversa dá o C-stick que percorre 70% do que falta neste quadro. O
 * giro não tem inércia (para assim que o C-stick solta). Se travar (limite vertical, pausa), o
 * resto é descartado. A referência aponta para trás da câmera: a vertical tem o sinal invertido. */
#define AIM_REF 0x80417308u

/* velocidade por quadro -> valor do C-stick, interpolando a tabela medida */
static float invert_curve(float rate, const float (*t)[2], int n) {
    if (rate <= 0) return 0;
    for (int i = 1; i < n; i++)
        if (rate <= t[i][0]) return t[i - 1][1] + (rate - t[i - 1][0]) / (t[i][0] - t[i - 1][0]) * (t[i][1] - t[i - 1][1]);
    return t[n - 1][1];
}

static void mouse_aim(u32 idx) {
    static const float yaw_curve[][2] = {{0, 0},         {0.0006f, 0.25f}, {0.0039f, 0.5f}, {0.045f, 1.0f},
                                         {0.064f, 2.0f}, {0.082f, 3.0f},   {0.089f, 6.0f}};
    static const float pitch_curve[][2] = {{0, 0},          {0.0008f, 0.25f}, {0.0032f, 0.5f}, {0.0127f, 1.0f},
                                           {0.052f, 2.0f}, {0.117f, 3.0f},   {0.2f, 4.0f}};
    static float sens = -1, sens_y, invert;
    static float pend_yaw, pend_pitch, last_yaw, last_pitch;
    static int have_last, stall_yaw, stall_pitch;
    if (sens < 0) {
        /* 4 = 0,0025 rad (~0,14 grau) por unidade do mouse */
        sens = (getenv("MA_MOUSE_SENS") ? (float)atof(getenv("MA_MOUSE_SENS")) : 4.0f) * 0.000625f;
        sens_y = getenv("MA_MOUSE_SENS_Y") ? (float)atof(getenv("MA_MOUSE_SENS_Y")) : 1.0f;
        invert = getenv("MA_MOUSE_INVERT") ? -1.0f : 1.0f;
    }
    float fx = rd_f32(AIM_REF), fy = rd_f32(AIM_REF + 4), fz = rd_f32(AIM_REF + 8);
    float n = fx * fx + fy * fy + fz * fz;
    if (!(n > 0.9f && n < 1.1f)) { /* sem mira de jogo (menus, carregamento) */
        pend_yaw = pend_pitch = 0;
        have_last = 0;
        g_aim_dx = g_aim_dy = 0;
        return;
    }
    float yaw = atan2f(fx, fz), pitch = -asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy);
    /* o que já girou desconta do que falta (só enquanto há algo pendente); passou do ponto, o
     * pendente troca de sinal e o C-stick corrige de volta */
    if (have_last) {
        float dy = wrap_pi(yaw - last_yaw), dp = pitch - last_pitch;
        if (pend_yaw != 0) {
            pend_yaw -= dy;
            stall_yaw = fabsf(dy) < 0.0001f ? stall_yaw + 1 : 0;
        }
        if (pend_pitch != 0) {
            pend_pitch -= dp;
            stall_pitch = fabsf(dp) < 0.0001f ? stall_pitch + 1 : 0;
        }
    }
    last_yaw = yaw;
    last_pitch = pitch;
    have_last = 1;
    /* comando = movimento do mouse neste quadro (direto, sem esperar medição) + 25% do erro que
     * sobrou dos quadros anteriores. Corrigir tudo pela medição oscilava: às vezes o jogo consome a
     * amostra um quadro depois e o controle pedia de novo, dobrando o passo. */
    float mov_yaw = (float)g_aim_dx * sens, mov_pitch = (float)-g_aim_dy * sens * sens_y * invert;
    g_aim_dx = g_aim_dy = 0;
    if (stall_yaw > 6 || fabsf(pend_yaw) < 0.0003f) { pend_yaw = 0; stall_yaw = 0; }
    if (stall_pitch > 6 || fabsf(pend_pitch) < 0.0003f) { pend_pitch = 0; stall_pitch = 0; }
    pend_yaw += mov_yaw;
    pend_pitch += mov_pitch;
    /* freio: o jogo tem velocidade máxima de giro (e ela começa baixa e acelera); numa puxada mais
     * rápida que isso, o excesso acumulado era cumprido depois de a mão parar e a mira seguia
     * girando sozinha. Acima de ~3,4 graus de atraso, o excesso é descartado. */
    const float max_backlog = 0.06f;
    if (pend_yaw > max_backlog) pend_yaw = max_backlog;
    if (pend_yaw < -max_backlog) pend_yaw = -max_backlog;
    if (pend_pitch > max_backlog) pend_pitch = max_backlog;
    if (pend_pitch < -max_backlog) pend_pitch = -max_backlog;
    /* com o mouse parado, a correção nunca volta contra o último movimento: se o jogo passou um
     * pouco do ponto (ele acelera no fim de uma puxada), a mira fica onde está, sem tranco */
    static float last_dir_yaw, last_dir_pitch;
    if (mov_yaw != 0) last_dir_yaw = mov_yaw;
    else if (pend_yaw * last_dir_yaw < 0) pend_yaw = 0;
    if (mov_pitch != 0) last_dir_pitch = mov_pitch;
    else if (pend_pitch * last_dir_pitch < 0) pend_pitch = 0;
    if (pend_yaw == 0 && pend_pitch == 0) return;
    /* erro dos quadros anteriores (já com freio e sem volta) */
    float sy = mov_yaw + 0.25f * (pend_yaw - mov_yaw), sp = mov_pitch + 0.25f * (pend_pitch - mov_pitch);

    u32 bufs = ld32(0x804B9888);
    if (!bufs) return;
    u32 rec = ld32(bufs) + idx * 0x50;
    if (ld32(rec) != 1) return; /* jogador 1 desconectado */
    float vy = invert_curve(fabsf(sy), yaw_curve, 7), vp = invert_curve(fabsf(sp), pitch_curve, 7);
    wr_f32(rec + 0x14, rd_f32(rec + 0x14) + (sy < 0 ? -vy : vy));
    wr_f32(rec + 0x18, rd_f32(rec + 0x18) + (sp < 0 ? -vp : vp));
}

/* Pad_SampleCallback (fn_802C0438): a rotina de amostragem do jogo lê o PADRead e grava, por
 * controle, um registro de 0x50 bytes em floats (ponteiros em 0x804B9888, índice da amostra em
 * 0x804B988C): +0x0C/+0x10 analógico, +0x14/+0x18 C-stick, já com zona morta, curva e limite
 * circular. Depois dela, mouse_aim soma ao C-stick do jogador 1 o comando da mira com o mouse. */
void hle_Pad_SampleCallback(PPCContext *c) {
    u32 idx = ld32(0x804B988C); /* registro que esta amostra preenche */
    f_802C0438_orig(c);
    /* diagnóstico: MA_AIM_TEST="início-fim:x:y,..." soma um valor fixo ao C-stick já processado */
    static const char *test;
    static int test_init;
    if (!test_init) { test = getenv("MA_AIM_TEST"); test_init = 1; }
    for (const char *p = test; p && *p;) {
        unsigned a = (unsigned)strtoul(p, (char **)&p, 10), e = a + 1;
        if (*p == '-') e = (unsigned)strtoul(p + 1, (char **)&p, 10);
        float tx = 0, ty = 0;
        if (*p == ':') tx = (float)strtod(p + 1, (char **)&p);
        if (*p == ':') ty = (float)strtod(p + 1, (char **)&p);
        if (*p == ',') p++;
        else p = "";
        u32 bufs = ld32(0x804B9888);
        if (g_retrace_count < a || g_retrace_count >= e || !bufs) continue;
        u32 rec = ld32(bufs) + idx * 0x50, w;
        float x, y;
        w = ld32(rec + 0x14); memcpy(&x, &w, 4); x += tx; memcpy(&w, &x, 4); st32(rec + 0x14, w);
        w = ld32(rec + 0x18); memcpy(&y, &w, 4); y += ty; memcpy(&w, &y, 4); st32(rec + 0x18, w);
    }
    mouse_aim(idx);
    /* diagnóstico: MA_TRACE="endereço,..." registra 3 floats de cada endereço a cada amostra */
    static const char *trace;
    static int trace_init;
    if (!trace_init) { trace = getenv("MA_TRACE"); trace_init = 1; }
    if (trace) {
        char line[512];
        int n = snprintf(line, sizeof line, "[trace] %u", g_retrace_count);
        for (const char *p = trace; *p && n < (int)sizeof line - 64;) {
            u32 a = (u32)strtoul(p, (char **)&p, 16);
            n += snprintf(line + n, sizeof line - n, "  %.4f %.4f %.4f", rd_f32(a), rd_f32(a + 4), rd_f32(a + 8));
            if (*p == ',') p++;
            else break;
        }
        LOG("%s", line);
    }
}

static int window_focused(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

/* Canal do GameCube -> controle XInput. Padrão: canal N = controle N, com teclado e mouse
 * somados ao canal 0. Com teclado_separado=1 (MA_KB_SEPARATE), teclado e mouse são o canal 0
 * sozinhos e os controles viram os canais 1 a 3 (uma pessoa no teclado, outra no controle). */
static int kb_separate(void) {
    static int v = -1;
    if (v < 0) v = getenv("MA_KB_SEPARATE") != NULL;
    return v;
}

static int xinput_for_channel(u32 ch) {
    int xi = kb_separate() ? (int)ch - 1 : (int)ch;
    return xi >= 0 && xi < 4 ? xi : -1;
}

/* XInputGetState num slot vazio é lento: controle ausente só é consultado de novo a cada ~2 s */
static int xinput_read(int xi, XState *xs) {
    static u32 retry_at[4];
    if (!g_xget || xi < 0) return 0;
    if (retry_at[xi] && g_retrace_count < retry_at[xi]) return 0;
    if (g_xget((DWORD)xi, xs) == 0) {
        retry_at[xi] = 0;
        return 1;
    }
    retry_at[xi] = g_retrace_count + 120;
    return 0;
}

void hle_PADInit(PPCContext *c) { xinput_load(); RET(1); }
void hle_PADReset(PPCContext *c) { RET(1); }
void hle_PADRecalibrate(PPCContext *c) { RET(1); }

/* PADControlMotor(canal, comando): 0 = parar, 1 = vibrar, 2 = parar forte */
void hle_PADControlMotor(PPCContext *c) {
    xinput_load();
    int xi = xinput_for_channel(ARG(0));
    if (!g_xset || xi < 0) return;
    XVibration v = {0, 0};
    if (ARG(1) == 1) v.wLeftMotorSpeed = v.wRightMotorSpeed = 40000;
    g_xset((DWORD)xi, &v);
}
/* O jogo registra aqui a própria rotina de leitura do controle (fn_802C0438); no
 * console ela roda a cada amostragem do SI, sincronizada com o vídeo. */
static u32 g_pad_sampling_cb;

void hle_PADSetSamplingCallback(PPCContext *c) {
    u32 old = g_pad_sampling_cb;
    g_pad_sampling_cb = ARG(0);
    RET(old);
}

void pad_retrace(PPCContext *c) {
    if (g_pad_sampling_cb) guest_callback(c, g_pad_sampling_cb, 0, 0, 0);
}

u32 g_pad_reads, g_pad_last_buttons;

void hle_PADRead(PPCContext *c) {
    u32 st = ARG(0);
    g_pad_reads++;
    for (u32 ch = 0; ch < 4; ch++, st += 12) {
        memset(host_ptr(st), 0, 12);
        int kb = ch == 0; /* teclado, mouse e roteiro de teste ficam no canal 0 */
        XState xs;
        int pad = xinput_read(xinput_for_channel(ch), &xs);
        if (!kb && !pad) {
            st8(st + 10, (u8)-1); /* PAD_ERR_NO_CONTROLLER */
            continue;
        }
        u16 b = kb ? scripted_buttons() : 0;
        s8 sx = kb ? g_script_sx : 0, sy = kb ? g_script_sy : 0, cx = kb ? g_script_cx : 0, cy = kb ? g_script_cy : 0;
        u8 tl = 0, tr = 0;

        /* controle XInput deste canal */
        if (pad) {
            WORD w = xs.Gamepad.wButtons;
            if (w & 0x1000) b |= PAD_A;     /* A */
            if (w & 0x2000) b |= PAD_B;     /* B */
            if (w & 0x4000) b |= PAD_X;     /* X */
            if (w & 0x8000) b |= PAD_Y;     /* Y */
            if (w & 0x0200) b |= PAD_Z;     /* RB */
            if (w & 0x0010) b |= PAD_START; /* Start */
            if (w & 0x0001) b |= PAD_UP;
            if (w & 0x0002) b |= PAD_DOWN;
            if (w & 0x0004) b |= PAD_LEFT;
            if (w & 0x0008) b |= PAD_RIGHT;
            sx = stick_axis(xs.Gamepad.sThumbLX, 7849);
            sy = stick_axis(xs.Gamepad.sThumbLY, 7849);
            cx = stick_axis(xs.Gamepad.sThumbRX, 8689);
            cy = stick_axis(xs.Gamepad.sThumbRY, 8689);
            tl = xs.Gamepad.bLeftTrigger;
            tr = xs.Gamepad.bRightTrigger;
            if (tl > 230) b |= PAD_L; /* clique digital no fim do curso, como no GameCube */
            if (tr > 230) b |= PAD_R;
        }

        /* teclado (só com a janela do jogo em foco) e botões do mouse (só capturado) */
        int mdx = 0, mdy = 0;
        if (kb) {
            g_mouse_captured_now = gx_mouse_poll(&mdx, &mdy, &g_mouse_l, &g_mouse_r);
            if (!g_mouse_captured_now) g_mouse_l = g_mouse_r = 0;
            g_focused_now = window_focused();
        }
        if (kb && (g_focused_now || g_mouse_captured_now)) {
            keymap_load();
            if (kdown(K_START)) b |= PAD_START;
            if (kdown(K_A)) b |= PAD_A;
            if (kdown(K_B)) b |= PAD_B;
            if (kdown(K_X)) b |= PAD_X;
            if (kdown(K_Y)) b |= PAD_Y;
            if (kdown(K_L)) { b |= PAD_L; tl = 255; }
            if (kdown(K_R)) { b |= PAD_R; tr = 255; }
            if (kdown(K_Z)) b |= PAD_Z;
            if (kdown(K_DUP)) b |= PAD_UP;
            if (kdown(K_DDOWN)) b |= PAD_DOWN;
            if (kdown(K_DLEFT)) b |= PAD_LEFT;
            if (kdown(K_DRIGHT)) b |= PAD_RIGHT;
            if (kdown(K_RIGHT)) sx = 100;
            if (kdown(K_LEFT)) sx = -100;
            if (kdown(K_UP)) sy = 100;
            if (kdown(K_DOWN)) sy = -100;
            if (kdown(K_CRIGHT)) cx = 100;
            if (kdown(K_CLEFT)) cx = -100;
            if (kdown(K_CUP)) cy = 100;
            if (kdown(K_CDOWN)) cy = -100;
        }
        /* mouse capturado: o movimento deste quadro vai direto para o C-stick já processado
         * pelo jogo (hle_Pad_SampleCallback), sem passar pela curva e pelo círculo do analógico */
        if (kb) {
            g_aim_dx = g_mouse_captured_now ? mdx : 0;
            g_aim_dy = g_mouse_captured_now ? mdy : 0;
        }
        st16(st, b);
        if (b) g_pad_last_buttons = b | (g_retrace_count << 16);
        st8(st + 2, (u8)sx);
        st8(st + 3, (u8)sy);
        st8(st + 4, (u8)cx);
        st8(st + 5, (u8)cy);
        st8(st + 6, tl);
        st8(st + 7, tr);
    }
    RET(0);
}

