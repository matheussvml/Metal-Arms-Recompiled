// Backend Direct3D 11 do GX: janela, EFB, shaders gerados a partir do TEV,
// cache de texturas e cópias do EFB.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "gx_internal.h"
}

namespace {

int kScale = 2;                    // resolução interna = EFB nativo x kScale (MA_SCALE=1..4)
float g_sx = 2;                    // escala horizontal: kScale, ou kScale x 4/3 no widescreen anamórfico
int g_vsync = 1;                   // MA_VSYNC=0 desliga a espera pelo monitor
int g_aniso = 8;                   // MA_ANISO: filtro anisotrópico nas texturas com mipmap (0/1 = desligado)
bool g_widescreen;                 // MA_WIDESCREEN: EFB mais largo, cena 3D e HUD comprimidos em x
int g_ws_squeeze;                  // o desenho atual tem x comprimido (widescreen)
constexpr int kEfbW = 640, kEfbH = 528;
constexpr int kWinW = 1280, kWinH = 960; // tamanho inicial da janela
int g_back_w = kWinW, g_back_h = kWinH;  // tamanho atual do back buffer
volatile int g_resize_w, g_resize_h;     // redimensionamento pendente (WM_SIZE)
bool g_borderless;
RECT g_windowed_rect;

HWND g_hwnd;
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
IDXGISwapChain *g_swap;
ID3D11RenderTargetView *g_back_rtv;
ID3D11Texture2D *g_efb_tex;
ID3D11RenderTargetView *g_efb_rtv;
ID3D11ShaderResourceView *g_efb_srv;
ID3D11DepthStencilView *g_efb_dsv;
ID3D11Buffer *g_vb, *g_ib, *g_ps_cb;
u32 g_ib_cap = 1 << 22, g_ib_pos;
ID3D11VertexShader *g_vs, *g_blit_vs;
ID3D11PixelShader *g_blit_ps, *g_copy_ps;
ID3D11ShaderResourceView *g_efb_zsrv;
ID3D11InputLayout *g_layout;
ID3D11SamplerState *g_blit_sampler;
u32 g_vb_cap = 1 << 22, g_vb_pos;
u64 g_frame;
int g_closed;

const char *kVertexShader = R"(
// um buffer por região da memória XF: só o que mudou é reenviado
cbuffer XPos : register(b0) { uint4 xpos[64]; };    // XF 0x000: matrizes de posição/textura
cbuffer XNrm : register(b1) { uint4 xnrm[24]; };    // XF 0x400: matrizes de normal (3x3)
cbuffer XPost : register(b2) { uint4 xpost[64]; };  // XF 0x500: pós-transformação (dual tex)
cbuffer XLight : register(b3) { uint4 xlight[32]; }; // XF 0x600: 8 luzes x 16 palavras
cbuffer XReg : register(b4) { uint4 xreg[24]; row_major float4x4 proj; }; // XF 0x1000
// entradas com 4 componentes: o shader indexa algumas dinamicamente, e o D3D
// completa os componentes que o layout não fornece
struct VSIn {
    float4 pos4 : POSITION; float4 nrm4 : NORMAL; float4 c0 : COLOR0; float4 c1 : COLOR1;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;
    uint4 pm4 : BLENDINDICES0; uint4 tm0 : BLENDINDICES1; uint4 tm1 : BLENDINDICES2;
};
struct VSOut {
    float4 pos : SV_Position; float4 c0 : COLOR0; float4 c1 : COLOR1;
    float3 t0 : TEXCOORD0; float3 t1 : TEXCOORD1; float3 t2 : TEXCOORD2; float3 t3 : TEXCOORD3;
    float3 t4 : TEXCOORD4; float3 t5 : TEXCOORD5; float3 t6 : TEXCOORD6; float3 t7 : TEXCOORD7;
};
uint R(uint k) { return xreg[k >> 2][k & 3]; }
float NW(uint k) { return asfloat(xnrm[k >> 2][k & 3]); }
uint LW(uint l, uint k) { return xlight[l * 4 + (k >> 2)][k & 3]; }
float LF(uint l, uint k) { return asfloat(LW(l, k)); }
float4 rgba(uint v) { return float4((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255) / 255.0; }

float4 lights(uint ctrl, float3 wp, float3 wn) {
    float4 acc = 0;
    uint mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
    uint dif = (ctrl >> 7) & 3, att = (ctrl >> 9) & 3;
    for (uint l = 0; l < 8; l++) {
        if (!(mask & (1u << l))) continue;
        float3 lpos = float3(LF(l, 10), LF(l, 11), LF(l, 12));
        float3 ldir = float3(LF(l, 13), LF(l, 14), LF(l, 15));
        float3 cosa = float3(LF(l, 4), LF(l, 5), LF(l, 6));
        float3 dista = float3(LF(l, 7), LF(l, 8), LF(l, 9));
        float3 v = lpos - wp;
        float a = 1;
        float3 L;
        if (att == 3) { // spot
            float d2 = dot(v, v), d = sqrt(d2);
            L = d > 0 ? v / d : wn;
            float c = max(0.0, dot(L, ldir));
            a = max(0.0, cosa.x + cosa.y * c + cosa.z * c * c) / max(1e-6, dot(dista, float3(1, d, d2)));
        } else if (att == 1) { // especular
            L = normalize(v);
            float c = dot(wn, L) >= 0 ? max(0.0, dot(wn, ldir)) : 0;
            float3 da = dif == 0 ? dista : normalize(dista);
            a = max(0.0, dot(cosa, float3(1, c, c * c))) / max(1e-6, dot(da, float3(1, c, c * c)));
        } else {
            L = length(v) > 0 ? normalize(v) : wn;
        }
        float nl = dot(L, wn);
        float f = dif == 0 ? 1 : dif == 1 ? nl : max(0.0, nl);
        acc += a * f * rgba(LW(l, 3));
    }
    return acc;
}

float4 channel(uint i, float4 vcol, float3 wp, float3 wn) {
    uint cc = R(0x0E + i), ac = R(0x10 + i);
    float4 matr = rgba(R(0x0C + i)), ambr = rgba(R(0x0A + i));
    float3 mat = (cc & 1) ? vcol.rgb : matr.rgb;
    float mata = (ac & 1) ? vcol.a : matr.a;
    float3 col = mat;
    float al = mata;
    if (cc & 2) {
        float3 amb = (cc & 64) ? vcol.rgb : ambr.rgb;
        col = mat * saturate(amb + lights(cc, wp, wn).rgb);
    }
    if (ac & 2) {
        float amb = (ac & 64) ? vcol.a : ambr.a;
        al = mata * saturate(amb + lights(ac, wp, wn).a);
    }
    return float4(col, al);
}

float3 texgen(uint i, float3 mpos, float3 mnrm, float4 vc0, float3 tin[8], uint tm[8], float4 c0) {
    uint info = R(0x40 + i);
    uint row = (info >> 7) & 31, type = (info >> 4) & 7;
    float4 src = float4(mpos, 1);
    if (row == 1) src = float4(mnrm, 1);
    else if (row == 2) src = vc0;
    else if (row >= 5 && row <= 12) src = float4(tin[row - 5].xy, 1, 1);
    float3 o;
    if (type == 2 || type == 3) {
        o = float3(c0.r, c0.g, 1);
    } else if (type == 1) {
        o = float3(src.xy, 1); // emboss: ainda não emulado
    } else {
        uint m = tm[i];
        o.x = dot(asfloat(xpos[m]), src);
        o.y = dot(asfloat(xpos[m + 1]), src);
        o.z = (info & 2) ? dot(asfloat(xpos[m + 2]), src) : 1;
    }
    if (R(0x12) & 1) {
        uint post = R(0x50 + i), p = post & 63;
        if (post & 256) o = normalize(o);
        float4 o4 = float4(o, 1);
        o = float3(dot(asfloat(xpost[p]), o4), dot(asfloat(xpost[p + 1]), o4), dot(asfloat(xpost[p + 2]), o4));
    }
    return o;
}

VSOut main(VSIn v) {
    VSOut o;
    uint pm = v.pm4.x;
    float3 mpos = v.pos4.xyz, mnrm = v.nrm4.xyz;
    float4 p4 = float4(mpos, 1);
    float3 wp = float3(dot(asfloat(xpos[pm]), p4), dot(asfloat(xpos[pm + 1]), p4), dot(asfloat(xpos[pm + 2]), p4));
    uint nb = (pm & 31) * 3;
    float3x3 nm = float3x3(NW(nb), NW(nb + 1), NW(nb + 2), NW(nb + 3), NW(nb + 4), NW(nb + 5),
                           NW(nb + 6), NW(nb + 7), NW(nb + 8));
    float3 wn = mul(nm, mnrm);
    wn = dot(wn, wn) > 0 ? normalize(wn) : float3(0, 0, 1);
    o.pos = mul(proj, float4(wp, 1));
    o.pos.z = o.pos.z + o.pos.w; // GX: z/w em [-1,0]; D3D: [0,1]
    o.c0 = channel(0, v.c0, wp, wn);
    o.c1 = channel(1, v.c1, wp, wn);
    float3 tin[8] = {v.t0.xyz, v.t1.xyz, v.t2.xyz, v.t3.xyz, v.t4.xyz, v.t5.xyz, v.t6.xyz, v.t7.xyz};
    uint tm[8] = {v.tm0.x, v.tm0.y, v.tm0.z, v.tm0.w, v.tm1.x, v.tm1.y, v.tm1.z, v.tm1.w};
    float3 t[8];
    uint ntex = R(0x3F);
    [unroll] for (uint i = 0; i < 8; i++) t[i] = i < ntex ? texgen(i, mpos, mnrm, v.c0, tin, tm, o.c0) : float3(0, 0, 1);
    o.t0 = t[0]; o.t1 = t[1]; o.t2 = t[2]; o.t3 = t[3]; o.t4 = t[4]; o.t5 = t[5]; o.t6 = t[6]; o.t7 = t[7];
    return o;
}
)";

const char *kBlitShader = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer B : register(b0) { float4 rect; };
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = rect.xy + p * rect.zw;
    return o;
}
float4 ps(O i) : SV_Target { return float4(src.Sample(smp, i.uv).rgb, 1); }
)";

// cópia do EFB para textura: converte para o formato pedido e devolve RGBA como o decodificador
// de texturas devolveria ao ler esse formato da RAM (I = intensidade em todos os canais, IA = I,I,I,A)
const char *kCopyShader = R"(
Texture2D src : register(t0);
Texture2D<float> zsrc : register(t1);
SamplerState smp : register(s0);
cbuffer B : register(b0) { float4 rect; uint4 mode; }; // x = formato, y = intensidade, z = origem (0 RGBA, 1 RGB, 2 Z)
float q(float x, float n) { return floor(x * n + 0.5) / n; }
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c;
    if (mode.z == 2) {
        uint z = (uint)round(saturate(zsrc.Sample(smp, uv)) * 16777215.0);
        c = float4((z >> 16) & 255, (z >> 8) & 255, z & 255, (z >> 8) & 255) / 255.0;
    } else {
        c = src.Sample(smp, uv);
        if (mode.z == 1) c.a = 1;
        if (mode.y) c.r = saturate(dot(c.rgb, float3(0.257, 0.504, 0.098)) + 16.0 / 255.0);
    }
    switch (mode.x) {
    case 0: return q(c.r, 15).xxxx;
    case 1: case 8: return c.rrrr;
    case 2: return float4(q(c.r, 15).xxx, q(c.a, 15));
    case 3: return float4(c.rrr, c.a);
    case 4: return float4(q(c.r, 31), q(c.g, 63), q(c.b, 31), 1);
    case 5: return c.a >= 0.875 ? float4(q(c.r, 31), q(c.g, 31), q(c.b, 31), 1)
                                : float4(q(c.r, 15), q(c.g, 15), q(c.b, 15), q(c.a, 7));
    case 7: return c.aaaa;
    case 9: return c.gggg;
    case 10: return c.bbbb;
    case 11: return float4(c.ggg, c.r);
    case 12: return float4(c.bbb, c.g);
    default: return c;
    }
}
)";

struct PSConst {
    float reg[4][4];
    float konst[4][4];
    float aref[4];
    float dstalpha[4];
    float fog[4];    // A, C, magnitude de B, deslocamento de B
    float fogcol[4];
    float tdim[8][4]; // xy = escala SU da coordenada, zw = tamanho do mapa
    float indm[6][4]; // linhas das matrizes indiretas; w = deslocamento (17 - escala)
};

// parâmetros de neblina (BP 0xEE/0xF1): float com mantissa de 11 bits, expoente 8, sinal
float fog_float(u32 v) {
    u32 bits = (((v >> 19) & 1) << 31) | (((v >> 11) & 0xFF) << 23) | ((v & 0x7FF) << 12);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

void fail(const char *what, HRESULT hr) { rt_fatal("D3D11: %s (hr=%08lX)", what, (unsigned long)hr); }

/* Cache de shaders em disco (shaders.cache ao lado do exe): hash do fonte HLSL + entrada + alvo
 * -> bytecode. Cada configuração nova do TEV compila um shader na hora (uma travadinha); com o
 * cache, isso só acontece na primeira vez em que o efeito aparece, não a cada execução.
 * Se o gerador mudar, o fonte muda junto e a entrada antiga só deixa de ser usada. */
const char kCacheMagic[8] = {'M', 'A', 'S', 'H', 'C', '0', '0', '1'};
std::unordered_map<u64, std::vector<u8>> g_shader_disk;
FILE *g_shader_file;
u32 g_shaders_compiled, g_shaders_cached;

u64 shader_key(const std::string &src, const char *entry, const char *target) {
    u64 h = 1469598103934665603ull;
    auto mix = [&h](const char *p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ (u8)p[i]) * 1099511628211ull;
        h = (h ^ 0xFF) * 1099511628211ull; // separador
    };
    mix(src.data(), src.size());
    mix(entry, strlen(entry));
    mix(target, strlen(target));
    return h;
}

void shader_cache_open() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *sl = strrchr(path, '\\');
    snprintf(sl ? sl + 1 : path, MAX_PATH - (sl ? (size_t)(sl + 1 - path) : 0), "shaders.cache");
    bool clean = false;
    if (FILE *f = fopen(path, "rb")) {
        char magic[8];
        clean = fread(magic, 1, 8, f) == 8 && !memcmp(magic, kCacheMagic, 8);
        while (clean) {
            u64 h;
            u32 n;
            if (fread(&h, 8, 1, f) != 1) break; // fim normal
            std::vector<u8> code;
            if (fread(&n, 4, 1, f) != 1 || n > (1u << 20) || (code.resize(n), fread(code.data(), 1, n, f) != n)) {
                clean = false; // registro cortado (o jogo foi fechado no meio da gravação)
                break;
            }
            g_shader_disk[h] = std::move(code);
        }
        fclose(f);
    }
    if (clean) {
        g_shader_file = fopen(path, "ab");
    } else { // arquivo novo ou estragado: regrava só o que é válido
        g_shader_file = fopen(path, "wb");
        if (g_shader_file) {
            fwrite(kCacheMagic, 1, 8, g_shader_file);
            for (auto &e : g_shader_disk) {
                u32 n = (u32)e.second.size();
                fwrite(&e.first, 8, 1, g_shader_file);
                fwrite(&n, 4, 1, g_shader_file);
                fwrite(e.second.data(), 1, n, g_shader_file);
            }
            fflush(g_shader_file);
        }
    }
    LOG("[gx] cache de shaders: %zu no disco", g_shader_disk.size());
}

ID3DBlob *compile(const std::string &src, const char *entry, const char *target) {
    u64 key = shader_key(src, entry, target);
    auto hit = g_shader_disk.find(key);
    if (hit != g_shader_disk.end()) {
        ID3DBlob *blob = nullptr;
        if (SUCCEEDED(D3DCreateBlob(hit->second.size(), &blob))) {
            memcpy(blob->GetBufferPointer(), hit->second.data(), hit->second.size());
            g_shaders_cached++;
            return blob;
        }
    }
    u64 t0 = tb_now();
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOG("erro de shader:\n%s\n---\n%s", err ? (const char *)err->GetBufferPointer() : "?", src.c_str());
        rt_fatal("compilação de shader falhou");
    }
    if (err) err->Release();
    g_shaders_compiled++;
    VLOG("[gx] shader %s compilado em %.1f ms", target, (double)(tb_now() - t0) * 1000.0 / TB_HZ);
    u32 n = (u32)code->GetBufferSize();
    const u8 *p = (const u8 *)code->GetBufferPointer();
    g_shader_disk[key].assign(p, p + n);
    if (g_shader_file) {
        fwrite(&key, 8, 1, g_shader_file);
        fwrite(&n, 4, 1, g_shader_file);
        fwrite(p, 1, n, g_shader_file);
        fflush(g_shader_file);
    }
    return code;
}

// F11: alterna entre janela e tela cheia sem bordas no monitor atual
void toggle_borderless(HWND h) {
    if (!g_borderless) {
        GetWindowRect(h, &g_windowed_rect);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(h, nullptr, g_windowed_rect.left, g_windowed_rect.top, g_windowed_rect.right - g_windowed_rect.left,
                     g_windowed_rect.bottom - g_windowed_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    g_borderless = !g_borderless;
}

// mira com o mouse: clicar na janela captura (cursor escondido e preso), Esc ou perder o foco solta
bool g_mouse_enabled = true, g_mouse_captured, g_mouse_swallow;
long g_mouse_dx, g_mouse_dy; // movimento acumulado desde a última leitura do controle

void mouse_clip(HWND h) {
    RECT r;
    GetClientRect(h, &r);
    MapWindowPoints(h, nullptr, (POINT *)&r, 2);
    ClipCursor(&r);
}

void mouse_capture(HWND h, bool on) {
    if (on == g_mouse_captured) return;
    g_mouse_captured = on;
    g_mouse_dx = g_mouse_dy = 0;
    if (on) {
        mouse_clip(h);
        while (ShowCursor(FALSE) >= 0) {}
        g_mouse_swallow = true; // o clique que capturou não vira tiro
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT && g_mouse_captured) {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_mouse_dx += ri.data.mouse.lLastX;
            g_mouse_dy += ri.data.mouse.lLastY;
        }
    }
    if (m == WM_LBUTTONDOWN && g_mouse_enabled && !g_mouse_captured) mouse_capture(h, true);
    // tecla abaixo do Esc (scan code 0x29: ' no ABNT2, ` no americano) mostra/esconde o mouse
    if (m == WM_KEYDOWN && ((l >> 16) & 0xFF) == 0x29 && !(l & (1 << 30)) && g_mouse_enabled)
        mouse_capture(h, !g_mouse_captured);
    if (m == WM_KILLFOCUS || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE)) mouse_capture(h, false);
    if ((m == WM_SIZE || m == WM_MOVE) && g_mouse_captured) mouse_clip(h);
    if (m == WM_CLOSE) {
        g_closed = 1;
        return 0;
    }
    if (m == WM_SIZE && LOWORD(l) > 0 && HIWORD(l) > 0) {
        g_resize_w = LOWORD(l);
        g_resize_h = HIWORD(l);
    }
    if (m == WM_KEYDOWN && w == VK_F11) {
        toggle_borderless(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- TEV -> HLSL ---------------------------------------------------------------

const char *kColorIn[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                            "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "float3(1,1,1)", "float3(0.5,0.5,0.5)",
                            "kon.rgb", "float3(0,0,0)"};
const char *kAlphaIn[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "kon.a", "0.0"};
const char *kDest[4] = {"prev", "c0", "c1", "c2"};

std::string konst_color(u32 sel) {
    static const char *frac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    char b[96];
    if (sel < 8) snprintf(b, sizeof b, "float3(%s,%s,%s)", frac[sel], frac[sel], frac[sel]);
    else if (sel >= 12 && sel < 16) snprintf(b, sizeof b, "K[%u].rgb", sel - 12);
    else if (sel >= 16) snprintf(b, sizeof b, "K[%u].%c%c%c", (sel - 16) & 3, "rgba"[(sel - 16) >> 2],
                                 "rgba"[(sel - 16) >> 2], "rgba"[(sel - 16) >> 2]);
    else snprintf(b, sizeof b, "float3(0,0,0)");
    return b;
}

std::string konst_alpha(u32 sel) {
    static const char *frac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    char b[64];
    if (sel < 8) snprintf(b, sizeof b, "%s", frac[sel]);
    else if (sel >= 16) snprintf(b, sizeof b, "K[%u].%c", (sel - 16) & 3, "rgba"[(sel - 16) >> 2]);
    else snprintf(b, sizeof b, "0.0");
    return b;
}

std::string swizzle(u32 table) {
    static const char c[4] = {'r', 'g', 'b', 'a'};
    u32 a = BP(0xF6 + 2 * table), b = BP(0xF7 + 2 * table);
    std::string s = ".";
    s += c[a & 3]; s += c[(a >> 2) & 3]; s += c[b & 3]; s += c[(b >> 2) & 3];
    return s;
}

const char *kCompare[8] = {"false", "(v < ref)", "(v == ref)", "(v <= ref)", "(v > ref)", "(v != ref)", "(v >= ref)", "true"};

// chave do pixel shader: tudo do BP que entra no HLSL gerado (sem alocar)
struct PSKey {
    u32 n;
    u32 v[1 + 16 * 5 + 8 + 1 + 3];
    bool operator==(const PSKey &o) const { return n == o.n && !memcmp(v, o.v, n * 4); }
};

u32 num_ind_stages() { return (BP(0x00) >> 16) & 7; }

void ps_key(PSKey *k) {
    u32 gm = BP(0x00);
    u32 ns = ((gm >> 10) & 15) + 1, n = 0, nind = num_ind_stages();
    k->v[n++] = gm & 0x73CFF; /* texgens, canais, estágios, culling, estágios indiretos */
    for (u32 s = 0; s < ns; s++) {
        k->v[n++] = BP(0xC0 + 2 * s);
        k->v[n++] = BP(0xC1 + 2 * s);
        k->v[n++] = (BP(0x28 + s / 2) >> (12 * (s & 1))) & 0xFFF;
        k->v[n++] = (BP(0xF6 + s / 2) >> ((s & 1) ? 14 : 4)) & 0x3FF;
        if (nind) k->v[n++] = BP(0x10 + s) & 0x1FFFFF; /* operação indireta do estágio */
    }
    for (u32 i = 0; i < 8; i++) k->v[n++] = BP(0xF6 + i) & 15;
    k->v[n++] = (BP(0xF3) & 0xFF0000) | (((BP(0xF1) >> 20) & 0xF) << 4); /* teste de alfa + neblina */
    if (nind) { /* IREF e escalas dos estágios indiretos */
        k->v[n++] = BP(0x27) & 0xFFFFFF;
        k->v[n++] = BP(0x25) & 0xFFFF;
        k->v[n++] = BP(0x26) & 0xFFFF;
    }
    k->n = n;
}

std::string gen_ps() {
    u32 gm = BP(0x00);
    u32 ns = ((gm >> 10) & 15) + 1;
    std::string s;
    s += "Texture2D T[8] : register(t0);\nSamplerState S[8] : register(s0);\n";
    s += "cbuffer P : register(b0) { float4 REG[4]; float4 K[4]; float4 AREF; float4 DSTA; float4 FOG; float4 FOGC;\n"
         " float4 TDIM[8]; float4 INDM[6]; };\n"; /* TDIM: xy = escala da coordenada (SU), zw = tamanho da textura */
    s += "struct VSOut { float4 pos : SV_Position; float4 c0 : COLOR0; float4 c1 : COLOR1;\n"
         " float3 t0 : TEXCOORD0; float3 t1 : TEXCOORD1; float3 t2 : TEXCOORD2; float3 t3 : TEXCOORD3;\n"
         " float3 t4 : TEXCOORD4; float3 t5 : TEXCOORD5; float3 t6 : TEXCOORD6; float3 t7 : TEXCOORD7; };\n";
    s += "int i8(float x) { return (int)round(saturate(x) * 255.0); }\n";
    s += "float4 main(VSOut i) : SV_Target {\n";
    s += " float3 tc[8] = {i.t0, i.t1, i.t2, i.t3, i.t4, i.t5, i.t6, i.t7};\n";
    s += " float4 prev = REG[0], c0 = REG[1], c1 = REG[2], c2 = REG[3];\n";
    s += " float4 tex = 0, ras = 0, kon = 0;\n";
    char b[640];

    /* texturas indiretas: coordenadas em ponto fixo (7 bits de fração) como no hardware */
    u32 nind = num_ind_stages();
    u32 ind_used = 0;
    for (u32 st = 0; st < ns && nind; st++) {
        u32 cmd = BP(0x10 + st);
        if ((cmd & 3) < nind && (((cmd >> 9) & 3) || ((cmd >> 7) & 3))) ind_used |= 1u << (cmd & 3);
    }
    if (nind) {
        s += " int2 fx[8];\n";
        for (u32 c = 0; c < 8; c++) {
            snprintf(b, sizeof b, " { float3 q = tc[%u]; fx[%u] = int2(floor(q.xy / (abs(q.z) > 1e-6 ? q.z : 1.0) * TDIM[%u].xy * 128.0)); }\n",
                     c, c, c);
            s += b;
        }
        s += " int2 tevcoord = int2(0, 0); int2 wrapped; int alphabump = 0;\n";
    }
    for (u32 ii = 0; ii < 4; ii++) {
        if (!(ind_used & (1u << ii))) continue;
        u32 iref = BP(0x27) >> (6 * ii);
        u32 imap = iref & 7, icoord = (iref >> 3) & 7;
        u32 ss = BP(0x25 + ii / 2) >> (8 * (ii & 1));
        snprintf(b, sizeof b,
                 " int3 iind%u = int3(round(T[%u].Sample(S[%u], float2(fx[%u] >> int2(%u, %u)) / (128.0 * TDIM[%u].zw)).abg * 255.0));\n",
                 ii, imap, imap, icoord, ss & 15, (ss >> 4) & 15, imap);
        s += b;
    }

    for (u32 st = 0; st < ns; st++) {
        u32 ce = BP(0xC0 + 2 * st), ae = BP(0xC1 + 2 * st);
        u32 tref = (BP(0x28 + st / 2) >> (12 * (st & 1))) & 0xFFF;
        u32 ks = (BP(0xF6 + st / 2) >> ((st & 1) ? 14 : 4)) & 0x3FF;
        u32 map = tref & 7, coord = (tref >> 3) & 7, en = (tref >> 6) & 1, chan = (tref >> 7) & 7;
        snprintf(b, sizeof b, " // estágio %u\n", st);
        s += b;
        u32 cmd = nind ? BP(0x10 + st) : 0;
        int indirect = nind && cmd != 0;
        if (indirect) {
            u32 bt = cmd & 3, fmt = (cmd >> 2) & 3, bias = (cmd >> 4) & 7, bs = (cmd >> 7) & 3;
            u32 mid = (cmd >> 9) & 3, mtype = (cmd >> 11) & 3, sw = (cmd >> 13) & 7, tw = (cmd >> 16) & 7;
            int has = bt < nind;
            if (has && bs) {
                static const char *sel[4] = {"", "x", "y", "z"};
                static const int ashift[4] = {0, 5, 4, 3};
                snprintf(b, sizeof b, " alphabump = (iind%u.%s << %d) & 248;\n", bt, sel[bs], ashift[fmt]);
                s += b;
            }
            s += " { int2 trans = int2(0, 0);\n";
            if (has && mid) {
                static const int fshift[4] = {0, 3, 4, 5};
                snprintf(b, sizeof b, "   int3 crd = iind%u >> %d;\n", bt, fshift[fmt]);
                s += b;
                const char *add = fmt == 0 ? "-128" : "1";
                if (bias & 1) { s += "   crd.x += "; s += add; s += ";\n"; }
                if (bias & 2) { s += "   crd.y += "; s += add; s += ";\n"; }
                if (bias & 4) { s += "   crd.z += "; s += add; s += ";\n"; }
                u32 m = 2 * (mid - 1);
                if (mtype == 0)
                    snprintf(b, sizeof b, "   trans = int2(dot(int3(INDM[%u].xyz), crd), dot(int3(INDM[%u].xyz), crd)) >> 3;\n", m, m + 1);
                else
                    snprintf(b, sizeof b, "   trans = int2(fx[%u] * crd.%s) >> 8;\n", coord, mtype == 1 ? "xx" : "yy");
                s += b;
                snprintf(b, sizeof b,
                         "   int w = (int)INDM[%u].w; if (w >= 0) trans >>= w; else trans <<= -w;\n", m);
                s += b;
            }
            static const char *wrapmask[5] = {"((256 << 7) - 1)", "((128 << 7) - 1)", "((64 << 7) - 1)", "((32 << 7) - 1)",
                                              "((16 << 7) - 1)"};
            snprintf(b, sizeof b, "   wrapped.x = %s; wrapped.y = %s;\n",
                     sw == 0 ? "fx[COORD].x" : sw >= 6 ? "0" : (std::string("fx[COORD].x & ") + wrapmask[sw - 1]).c_str(),
                     tw == 0 ? "fx[COORD].y" : tw >= 6 ? "0" : (std::string("fx[COORD].y & ") + wrapmask[tw - 1]).c_str());
            std::string line = b;
            for (size_t p; (p = line.find("COORD")) != std::string::npos;) line.replace(p, 5, std::to_string(coord));
            s += line;
            s += (cmd >> 20) & 1 ? "   tevcoord += wrapped + trans;\n" : "   tevcoord = wrapped + trans;\n";
            s += "   tevcoord = (tevcoord << 8) >> 8; }\n";
        }
        if (en && indirect) {
            snprintf(b, sizeof b, " tex = T[%u].Sample(S[%u], float2(tevcoord) / (128.0 * TDIM[%u].zw))%s;\n", map, map,
                     map, swizzle((ae >> 2) & 3).c_str());
            s += b;
        } else if (en) {
            snprintf(b, sizeof b,
                     " { float3 q = tc[%u]; float2 uv = q.xy / (abs(q.z) > 1e-6 ? q.z : 1.0);"
                     " tex = T[%u].Sample(S[%u], uv)%s; }\n",
                     coord, map, map, swizzle((ae >> 2) & 3).c_str());
            s += b;
        } else {
            s += " tex = 0;\n";
        }
        const char *rs = chan == 0 ? "i.c0" : chan == 1 ? "i.c1"
                       : chan == 5 && nind ? "float4(alphabump, alphabump, alphabump, alphabump) / 255.0"
                       : chan == 6 && nind ? "float4(alphabump | (alphabump >> 5), alphabump | (alphabump >> 5), alphabump | (alphabump >> 5), alphabump | (alphabump >> 5)) / 255.0"
                                           : "float4(0,0,0,0)";
        snprintf(b, sizeof b, " ras = %s%s;\n", rs, swizzle(ae & 3).c_str());
        s += b;
        snprintf(b, sizeof b, " kon = float4(%s, %s);\n", konst_color(ks & 31).c_str(), konst_alpha((ks >> 5) & 31).c_str());
        s += b;
        // cor
        {
            u32 d = ce & 15, c = (ce >> 4) & 15, bb = (ce >> 8) & 15, a = (ce >> 12) & 15;
            u32 bias = (ce >> 16) & 3, op = (ce >> 18) & 1, clamp = (ce >> 19) & 1, scale = (ce >> 20) & 3,
                dest = (ce >> 22) & 3;
            snprintf(b, sizeof b, " { float3 A = saturate(%s), B = saturate(%s), C = saturate(%s), D = %s; float3 r;\n",
                     kColorIn[a], kColorIn[bb], kColorIn[c], kColorIn[d]);
            s += b;
            if (bias == 3) {
                u32 mode = op | (scale << 1);
                const char *cmp;
                switch (mode >> 1) {
                case 0: cmp = "i8(A.r) %s i8(B.r)"; break;
                case 1: cmp = "(i8(A.g)*256 + i8(A.r)) %s (i8(B.g)*256 + i8(B.r))"; break;
                case 2: cmp = "(i8(A.b)*65536 + i8(A.g)*256 + i8(A.r)) %s (i8(B.b)*65536 + i8(B.g)*256 + i8(B.r))"; break;
                default: cmp = nullptr; break;
                }
                const char *rel = (mode & 1) ? "==" : ">";
                if (cmp) {
                    char e[256];
                    snprintf(e, sizeof e, cmp, rel, rel);
                    snprintf(b, sizeof b, "  r = D + ((%s) ? C : 0);\n", e);
                } else {
                    snprintf(b, sizeof b,
                             "  r = D + float3(i8(A.r) %s i8(B.r) ? C.r : 0, i8(A.g) %s i8(B.g) ? C.g : 0, i8(A.b) %s i8(B.b) ? C.b : 0);\n",
                             rel, rel, rel);
                }
                s += b;
            } else {
                static const char *bs[3] = {"0.0", "0.5", "-0.5"};
                static const char *sc[4] = {"1.0", "2.0", "4.0", "0.5"};
                snprintf(b, sizeof b, "  r = (D %s lerp(A, B, C) + %s) * %s;\n", op ? "-" : "+", bs[bias], sc[scale]);
                s += b;
            }
            snprintf(b, sizeof b, "  %s.rgb = %s; }\n", kDest[dest],
                     clamp ? "saturate(r)" : "clamp(r, -1024.0/255.0, 1023.0/255.0)");
            s += b;
        }
        // alfa
        {
            u32 d = (ae >> 4) & 7, c = (ae >> 7) & 7, bb = (ae >> 10) & 7, a = (ae >> 13) & 7;
            u32 bias = (ae >> 16) & 3, op = (ae >> 18) & 1, clamp = (ae >> 19) & 1, scale = (ae >> 20) & 3,
                dest = (ae >> 22) & 3;
            snprintf(b, sizeof b, " { float A = saturate(%s), B = saturate(%s), C = saturate(%s), D = %s; float r;\n",
                     kAlphaIn[a], kAlphaIn[bb], kAlphaIn[c], kAlphaIn[d]);
            s += b;
            if (bias == 3) {
                const char *rel = op ? "==" : ">";
                snprintf(b, sizeof b, "  r = D + ((i8(A) %s i8(B)) ? C : 0);\n", rel);
            } else {
                static const char *bs[3] = {"0.0", "0.5", "-0.5"};
                static const char *sc[4] = {"1.0", "2.0", "4.0", "0.5"};
                snprintf(b, sizeof b, "  r = (D %s lerp(A, B, C) + %s) * %s;\n", op ? "-" : "+", bs[bias], sc[scale]);
            }
            s += b;
            snprintf(b, sizeof b, "  %s.a = %s; }\n", kDest[dest],
                     clamp ? "saturate(r)" : "clamp(r, -1024.0/255.0, 1023.0/255.0)");
            s += b;
        }
    }
    // teste de alfa
    u32 ac = BP(0xF3);
    u32 comp0 = (ac >> 16) & 7, comp1 = (ac >> 19) & 7, logic = (ac >> 22) & 3;
    if (!(comp0 == 7 && comp1 == 7)) {
        std::string e0 = kCompare[comp0], e1 = kCompare[comp1];
        auto subst = [](std::string e, const char *ref) {
            size_t p;
            while ((p = e.find("ref")) != std::string::npos) e.replace(p, 3, ref);
            while ((p = e.find("v ")) != std::string::npos) e.replace(p, 2, "va ");
            return e;
        };
        e0 = subst(e0, "AREF.x");
        e1 = subst(e1, "AREF.y");
        static const char *lg[4] = {"&&", "||", "!=", "=="};
        snprintf(b, sizeof b, " { int va = i8(prev.a); bool p0 = %s, p1 = %s; if (!(p0 %s p1)) discard; }\n",
                 e0.c_str(), e1.c_str(), lg[logic]);
        s += b;
    }
    // o alfa constante do PE (GXSetDstAlpha) é aplicado no blend, não aqui:
    // a mistura de cor continua usando o alfa do TEV
    // neblina (BP 0xF1: tipo nos bits 21-23, projeção ortográfica no bit 20)
    u32 fp3 = BP(0xF1), fsel = (fp3 >> 21) & 7;
    if (fsel >= 2) {
        s += " { float zc = round(i.pos.z * 16777215.0);\n";
        if ((fp3 >> 20) & 1) s += "   float ze = FOG.x * zc / 16777216.0;\n";
        else s += "   float ze = (FOG.x * 16777216.0) / (FOG.z - floor(zc / exp2(FOG.w)));\n";
        s += "   float f = saturate(ze - FOG.y);\n";
        switch (fsel) {
        case 4: s += "   f = 1.0 - exp2(-8.0 * f);\n"; break;
        case 5: s += "   f = 1.0 - exp2(-8.0 * f * f);\n"; break;
        case 6: s += "   f = exp2(-8.0 * (1.0 - f));\n"; break;
        case 7: s += "   f = 1.0 - f; f = exp2(-8.0 * f * f);\n"; break;
        default: break; // 2 = linear
        }
        s += "   prev.rgb = lerp(saturate(prev.rgb), FOGC.rgb, f); }\n";
    }
    s += " return saturate(prev);\n}\n";
    return s;
}

struct KeyHash {
    size_t operator()(const PSKey &k) const {
        u64 h = 1469598103934665603ull;
        for (u32 i = 0; i < k.n; i++) h = (h ^ k.v[i]) * 1099511628211ull;
        return (size_t)h;
    }
};
std::unordered_map<PSKey, ID3D11PixelShader *, KeyHash> g_ps_cache;

// grupos de registradores BP (reg >> 3) que entram no pixel shader
const u32 kTevGroups = BPG(0x00) | BPG(0x10) | BPG(0x18) | BPG(0x20) | BPG(0x28) | BPG(0xC0) | BPG(0xC8) | BPG(0xD0) | BPG(0xD8) | BPG(0xF0) | BPG(0xF8);

ID3D11PixelShader *get_ps() {
    static ID3D11PixelShader *last;
    if (last && !(g_bp_dirty & kTevGroups)) return last;
    PSKey key;
    ps_key(&key);
    auto it = g_ps_cache.find(key);
    if (it != g_ps_cache.end()) return last = it->second;
    std::string src = gen_ps();
    ID3DBlob *code = compile(src, "main", "ps_5_0");
    ID3D11PixelShader *ps;
    HRESULT hr = g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    code->Release();
    if (FAILED(hr)) fail("CreatePixelShader", hr);
    g_ps_cache[key] = ps;
    VLOG("[gx] pixel shader #%zu (%u estágios)", g_ps_cache.size(), ((BP(0) >> 10) & 15) + 1);
    return last = ps;
}

// ---- texturas -------------------------------------------------------------------

struct TexEntry {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    u64 hash = 0;
    u64 checked_frame = ~0ull;
};
std::unordered_map<u64, TexEntry> g_tex_cache;

struct EfbCopy {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    u32 w = 0, h = 0;
    u64 mem_hash = 0;
};
std::unordered_map<u32, EfbCopy> g_efb_copies;

u64 hash_bytes(const u8 *p, u32 n) {
    u64 h = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::vector<u8> g_decode_buf;
u64 g_tex_decoded;

ID3D11ShaderResourceView *get_texture(u32 map) {
    u32 i0 = map < 4 ? 0x88 + map : 0xA8 + map - 4;
    u32 i3 = map < 4 ? 0x94 + map : 0xB4 + map - 4;
    u32 tl = map < 4 ? 0x98 + map : 0xB8 + map - 4;
    u32 img0 = BP(i0);
    u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    u32 addr = (BP(i3) & 0xFFFFFF) << 5;
    u32 tlut_addr = (BP(tl) & 0x3FF) << 9, tlut_fmt = (BP(tl) >> 10) & 3;
    const u8 *src = host_ptr(addr);

    /* mipmaps: níveis guardados em sequência depois do nível 0 */
    u32 mode0 = BP(map < 4 ? 0x80 + map : 0xA0 + map - 4), mode1 = BP(map < 4 ? 0x84 + map : 0xA4 + map - 4);
    u32 minf = (mode0 >> 5) & 7, levels = 1;
    if (minf >= 2 && minf <= 5) {
        u32 max_by_lod = ((mode1 >> 8) & 0xFF) / 16 + 1, max_by_size = 1;
        for (u32 d = w > h ? w : h; d > 1; d >>= 1) max_by_size++;
        levels = max_by_lod < max_by_size ? max_by_lod : max_by_size;
        if (levels > 11) levels = 11;
    }
    u32 size = 0;
    for (u32 l = 0; l < levels; l++) {
        u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        size += gx_texture_size(lw, lh, fmt);
    }

    auto ec = g_efb_copies.find(addr & 0x01FFFFFF);
    if (ec != g_efb_copies.end() && hash_bytes(src, size < 4096 ? size : 4096) == ec->second.mem_hash)
        return ec->second.srv;

    u64 key = ((u64)addr << 32) ^ ((u64)w << 20) ^ ((u64)h << 8) ^ fmt ^ ((u64)tlut_addr << 40) ^
              ((u64)tlut_fmt << 62) ^ ((u64)levels << 52);
    TexEntry &e = g_tex_cache[key];
    if (e.checked_frame == g_frame && e.srv) return e.srv;
    u64 hsh = hash_bytes(src, size);
    if (fmt >= 8 && fmt <= 10) hsh ^= hash_bytes(g_tmem + tlut_addr, fmt == 8 ? 32 : 512) * 31;
    e.checked_frame = g_frame;
    if (e.srv && e.hash == hsh) return e.srv;
    e.hash = hsh;

    /* decodifica todos os níveis num buffer só */
    D3D11_SUBRESOURCE_DATA sd[11];
    size_t total = 0;
    for (u32 l = 0; l < levels; l++) {
        u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        total += (size_t)lw * lh * 4;
    }
    g_decode_buf.resize(total);
    size_t out = 0;
    u32 in = 0;
    for (u32 l = 0; l < levels; l++) {
        u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        if (!gx_decode_texture(g_decode_buf.data() + out, src + in, lw, lh, fmt, tlut_addr, tlut_fmt)) {
            static int warned;
            if (warned++ < 10) LOG("[gx] formato de textura %u não suportado", fmt);
            memset(g_decode_buf.data() + out, 255, (size_t)lw * lh * 4);
        }
        sd[l].pSysMem = g_decode_buf.data() + out;
        sd[l].SysMemPitch = lw * 4;
        sd[l].SysMemSlicePitch = 0;
        out += (size_t)lw * lh * 4;
        in += gx_texture_size(lw, lh, fmt);
    }
    g_tex_decoded++;
    if (e.tex) {
        D3D11_TEXTURE2D_DESC d;
        e.tex->GetDesc(&d);
        if (d.Width == w && d.Height == h && d.MipLevels == levels) {
            for (u32 l = 0; l < levels; l++)
                g_ctx->UpdateSubresource(e.tex, l, nullptr, sd[l].pSysMem, sd[l].SysMemPitch, 0);
            return e.srv;
        }
        e.srv->Release();
        e.tex->Release();
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = levels; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_dev->CreateTexture2D(&td, sd, &e.tex);
    if (FAILED(hr)) fail("CreateTexture2D", hr);
    g_dev->CreateShaderResourceView(e.tex, nullptr, &e.srv);
    return e.srv;
}

std::unordered_map<u64, ID3D11SamplerState *> g_samplers;

ID3D11SamplerState *get_sampler(u32 map) {
    u32 m0 = BP(map < 4 ? 0x80 + map : 0xA0 + map - 4), m1 = BP(map < 4 ? 0x84 + map : 0xA4 + map - 4);
    u64 key = (u64)(m0 & 0x1FFFF) | ((u64)(m1 & 0xFFFF) << 32);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    static const D3D11_TEXTURE_ADDRESS_MODE wrap[4] = {D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP,
                                                       D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_WRAP};
    D3D11_SAMPLER_DESC sd = {};
    /* filtro de minificação do GX: 0 ponto, 1 linear, 2-5 = (ponto|linear) x mip (ponto|linear) */
    u32 minf = (m0 >> 5) & 7;
    int mag_lin = (m0 >> 4) & 1, min_lin = minf == 1 || minf == 3 || minf == 5, mip_lin = minf == 4 || minf == 5;
    sd.Filter = D3D11_ENCODE_BASIC_FILTER(min_lin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                          mag_lin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                          mip_lin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                          D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    sd.AddressU = wrap[m0 & 3];
    sd.AddressV = wrap[(m0 >> 2) & 3];
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias = (float)(s8)((m0 >> 9) & 0xFF) / 32.0f;
    sd.MinLOD = (float)(m1 & 0xFF) / 16.0f;
    sd.MaxLOD = minf >= 2 && minf <= 5 ? (float)((m1 >> 8) & 0xFF) / 16.0f : 0.0f;
    if (g_aniso > 1 && min_lin) { // melhoria do port: chão e paredes vistos de lado sem borrão nem cintilação
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = (UINT)g_aniso;
    }
    ID3D11SamplerState *s;
    g_dev->CreateSamplerState(&sd, &s);
    g_samplers[key] = s;
    return s;
}

// ---- estados fixos ------------------------------------------------------------------

std::unordered_map<u32, ID3D11BlendState *> g_blend;
std::unordered_map<u32, ID3D11DepthStencilState *> g_depth;
std::unordered_map<u32, ID3D11RasterizerState *> g_raster;

int efb_has_alpha() { return (BP(0x43) & 7) == 1; }

ID3D11BlendState *get_blend() {
    u32 cm = BP(0x41);
    int dst_alpha = (BP(0x42) >> 8) & 1;
    u32 key = (cm & 0xFFF) | (efb_has_alpha() << 16) | (dst_alpha << 17);
    auto it = g_blend.find(key);
    if (it != g_blend.end()) return it->second;
    static const D3D11_BLEND srcf[8] = {D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
                                        D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA};
    static const D3D11_BLEND dstf[8] = {D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR,
                                        D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA};
    auto to_alpha = [](D3D11_BLEND b) {
        switch (b) {
        case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
        case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
        default: return b;
        }
    };
    D3D11_BLEND_DESC bd = {};
    D3D11_RENDER_TARGET_BLEND_DESC &rt = bd.RenderTarget[0];
    int blend = cm & 1, sub = (cm >> 11) & 1;
    rt.BlendEnable = blend || sub;
    if (sub) {
        rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
        rt.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
    } else {
        rt.SrcBlend = srcf[(cm >> 8) & 7];
        rt.DestBlend = dstf[(cm >> 5) & 7];
        rt.BlendOp = D3D11_BLEND_OP_ADD;
    }
    rt.SrcBlendAlpha = to_alpha(rt.SrcBlend);
    rt.DestBlendAlpha = to_alpha(rt.DestBlend);
    rt.BlendOpAlpha = rt.BlendOp;
    if (!rt.BlendEnable) {
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    if (dst_alpha) { // GXSetDstAlpha: o EFB recebe a constante (passada como blend factor)
        rt.BlendEnable = TRUE;
        rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    UINT8 mask = 0;
    if (cm & 8) mask |= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if ((cm & 16) && efb_has_alpha()) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    rt.RenderTargetWriteMask = mask;
    ID3D11BlendState *s;
    g_dev->CreateBlendState(&bd, &s);
    g_blend[key] = s;
    return s;
}

ID3D11DepthStencilState *get_depth() {
    u32 z = BP(0x40) & 0x1F;
    auto it = g_depth.find(z);
    if (it != g_depth.end()) return it->second;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthFunc = (z & 1) ? (D3D11_COMPARISON_FUNC)(((z >> 1) & 7) + 1) : D3D11_COMPARISON_ALWAYS;
    dd.DepthWriteMask = (z & 1) && (z & 16) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    ID3D11DepthStencilState *s;
    g_dev->CreateDepthStencilState(&dd, &s);
    g_depth[z] = s;
    return s;
}

ID3D11RasterizerState *get_raster(int prim) {
    u32 cull = (BP(0x00) >> 14) & 3;
    if (prim != 2) cull = 0;
    auto it = g_raster.find(cull);
    if (it != g_raster.end()) return it->second;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = cull == 1 ? D3D11_CULL_BACK : cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    // registrador do GX: 1 = descarta trás, 2 = descarta frente; frente = horário na tela
    // (conferido na tela título: com anti-horário os elementos 2D sumiam)
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    ID3D11RasterizerState *s;
    g_dev->CreateRasterizerState(&rd, &s);
    g_raster[cull] = s;
    return s;
}

// ---- constantes por desenho --------------------------------------------------------

// regiões da XF -> constant buffers b0..b4 do vertex shader
struct XfBlock { u32 xf_addr, words, dirty_bit; };
const XfBlock kXfBlocks[5] = {
    {0x000, 64 * 4, XFD_POS}, {0x400, 24 * 4, XFD_NRM}, {0x500, 64 * 4, XFD_POST},
    {0x600, 32 * 4, XFD_LIGHT}, {0x1000, 24 * 4, XFD_REG},
};
ID3D11Buffer *g_xf_cb[5];
ID3D11Buffer *g_blit_cb;
int g_force_state = 1; // depois de apresentar/copiar, tudo é reenviado no próximo desenho

void build_proj(float m[16]) {
    float p[6];
    for (int i = 0; i < 6; i++) p[i] = XFF(0x1020 + i);
    memset(m, 0, 16 * sizeof(float));
    if (XF(0x1026) & 1) { // ortográfica
        m[0] = p[0]; m[3] = p[1]; m[5] = p[2]; m[7] = p[3]; m[10] = p[4]; m[11] = p[5]; m[15] = 1;
    } else {
        m[0] = p[0]; m[2] = p[1]; m[5] = p[2]; m[6] = p[3]; m[10] = p[4]; m[11] = p[5]; m[14] = -1;
    }
    if (g_ws_squeeze) { // widescreen: x do clip × 3/4 (mais campo de visão na cena, HUD no centro em 4:3)
        for (int k = 0; k < 4; k++) m[k] *= 0.75f;
    }
}

void upload_constants() {
    D3D11_MAPPED_SUBRESOURCE ms;
    for (int b = 0; b < 5; b++) {
        if (!g_force_state && !(g_xf_dirty & kXfBlocks[b].dirty_bit)) continue;
        g_ctx->Map(g_xf_cb[b], 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        memcpy(ms.pData, &g_gx.xfmem[kXfBlocks[b].xf_addr], kXfBlocks[b].words * 4);
        if (b == 4) build_proj((float *)((u8 *)ms.pData + kXfBlocks[b].words * 4));
        g_ctx->Unmap(g_xf_cb[b], 0);
    }
    const u32 ps_groups = BPG(0xE0) | BPG(0xE8) | BPG(0xF0) | BPG(0x40) | BPG(0x00) | BPG(0x08) | BPG(0x30) |
                          BPG(0x38) | BPG(0x88) | BPG(0xA8);
    if (!g_force_state && !(g_bp_dirty & ps_groups)) return;

    g_ctx->Map(g_ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    PSConst *p = (PSConst *)ms.pData;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            p->reg[r][k] = g_tev_reg[r][k] / 255.0f;
            p->konst[r][k] = g_tev_konst[r][k] / 255.0f;
        }
    p->aref[0] = (float)(BP(0xF3) & 0xFF);
    p->aref[1] = (float)((BP(0xF3) >> 8) & 0xFF);
    p->dstalpha[0] = (BP(0x42) & 0xFF) / 255.0f;
    p->fog[0] = fog_float(BP(0xEE));
    p->fog[1] = fog_float(BP(0xF1));
    p->fog[2] = (float)(BP(0xEF) & 0xFFFFFF);
    p->fog[3] = (float)(BP(0xF0) & 0x1F);
    p->fogcol[0] = ((BP(0xF2) >> 16) & 0xFF) / 255.0f;
    p->fogcol[1] = ((BP(0xF2) >> 8) & 0xFF) / 255.0f;
    p->fogcol[2] = (BP(0xF2) & 0xFF) / 255.0f;
    for (u32 c = 0; c < 8; c++) {
        u32 img0 = BP(c < 4 ? 0x88 + c : 0xA8 + c - 4);
        p->tdim[c][0] = (float)((BP(0x30 + 2 * c) & 0xFFFF) + 1);
        p->tdim[c][1] = (float)((BP(0x31 + 2 * c) & 0xFFFF) + 1);
        p->tdim[c][2] = (float)((img0 & 0x3FF) + 1);
        p->tdim[c][3] = (float)(((img0 >> 10) & 0x3FF) + 1);
    }
    for (u32 m = 0; m < 3; m++) {
        u32 a = BP(0x06 + 3 * m), b = BP(0x07 + 3 * m), c = BP(0x08 + 3 * m);
        auto s11 = [](u32 v) { return (float)(((int)(v << 21)) >> 21); };
        int scale = (int)(((a >> 22) & 3) | (((b >> 22) & 3) << 2) | (((c >> 22) & 3) << 4));
        float *r0 = p->indm[2 * m], *r1 = p->indm[2 * m + 1];
        r0[0] = s11(a); r0[1] = s11(b); r0[2] = s11(c); r0[3] = (float)(17 - scale);
        r1[0] = s11(a >> 11); r1[1] = s11(b >> 11); r1[2] = s11(c >> 11); r1[3] = (float)(17 - scale);
    }
    g_ctx->Unmap(g_ps_cb, 0);
}

void set_viewport_scissor() {
    float soffx = (float)((BP(0x59) & 0x3FF) * 2), soffy = (float)(((BP(0x59) >> 10) & 0x3FF) * 2);
    float wd = XFF(0x101A), ht = XFF(0x101B), zr = XFF(0x101C), xo = XFF(0x101D), yo = XFF(0x101E), fz = XFF(0x101F);
    D3D11_VIEWPORT vp;
    vp.TopLeftX = (xo - wd - soffx) * g_sx;
    vp.TopLeftY = (yo + ht - soffy) * kScale;
    vp.Width = 2 * wd * g_sx;
    vp.Height = -2 * ht * kScale;
    if (vp.Width < 0) { vp.TopLeftX += vp.Width; vp.Width = -vp.Width; }
    if (vp.Height < 0) { vp.TopLeftY += vp.Height; vp.Height = -vp.Height; }
    float n = (fz - zr) / 16777215.0f, f = fz / 16777215.0f;
    vp.MinDepth = n < 0 ? 0 : n > 1 ? 1 : n;
    vp.MaxDepth = f < 0 ? 0 : f > 1 ? 1 : f;
    static D3D11_VIEWPORT last_vp;
    static D3D11_RECT last_sc;
    if (g_force_state || memcmp(&vp, &last_vp, sizeof vp)) {
        g_ctx->RSSetViewports(1, &vp);
        last_vp = vp;
    }
    u32 tl = BP(0x20), br = BP(0x21);
    D3D11_RECT r;
    r.left = (LONG)((((tl >> 12) & 0x7FF) - soffx) * g_sx);
    r.top = (LONG)((tl & 0x7FF) - soffy) * kScale;
    r.right = (LONG)((((br >> 12) & 0x7FF) - soffx + 1) * g_sx);
    r.bottom = (LONG)((br & 0x7FF) - soffy + 1) * kScale;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (g_force_state || memcmp(&r, &last_sc, sizeof r)) {
        g_ctx->RSSetScissorRects(1, &r);
        last_sc = r;
    }
}

// ---- PNG para capturas ----------------------------------------------------------------

u32 crc_table[256];
u32 crc32(const u8 *p, size_t n, u32 c = 0xFFFFFFFFu) {
    if (!crc_table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

void write_png(const char *path, const u8 *rgba, u32 w, u32 h, u32 pitch) {
    std::vector<u8> raw;
    for (u32 y = 0; y < h; y++) {
        raw.push_back(0);
        for (u32 x = 0; x < w; x++) {
            const u8 *p = rgba + y * pitch + x * 4;
            raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]);
        }
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b2 = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b2 = (b2 + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((u8)n); z.push_back((u8)(n >> 8)); z.push_back((u8)~n); z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    u32 ad = (b2 << 16) | a;
    z.push_back((u8)(ad >> 24)); z.push_back((u8)(ad >> 16)); z.push_back((u8)(ad >> 8)); z.push_back((u8)ad);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    auto chunk = [&](const char *type, const std::vector<u8> &d) {
        u8 len[4] = {(u8)(d.size() >> 24), (u8)(d.size() >> 16), (u8)(d.size() >> 8), (u8)d.size()};
        fwrite(len, 1, 4, f);
        std::vector<u8> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        fwrite(td.data(), 1, td.size(), f);
        u32 c = ~crc32(td.data(), td.size());
        u8 cb[4] = {(u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c};
        fwrite(cb, 1, 4, f);
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<u8> ihdr = {(u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16),
                            (u8)(h >> 8), (u8)h, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

void save_texture(const char *path, ID3D11Texture2D *bb) {
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st;
    g_dev->CreateTexture2D(&d, nullptr, &st);
    g_ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
        write_png(path, (const u8 *)ms.pData, d.Width, d.Height, ms.RowPitch);
        g_ctx->Unmap(st, 0);
    }
    st->Release();
}

void screenshot(const char *path) {
    ID3D11Texture2D *bb;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    save_texture(path, bb);
    bb->Release();
}

} // namespace

// ---- interface com o interpretador ----------------------------------------------------

extern "C" int gxb_init(void) {
    if (getenv("MA_SCALE")) {
        int s = atoi(getenv("MA_SCALE"));
        kScale = s < 1 ? 1 : s > 4 ? 4 : s;
    }
    g_widescreen = getenv("MA_WIDESCREEN") != nullptr;
    if (getenv("MA_VSYNC")) g_vsync = atoi(getenv("MA_VSYNC")) != 0;
    if (getenv("MA_ANISO")) {
        int a = atoi(getenv("MA_ANISO"));
        g_aniso = a < 1 ? 1 : a > 16 ? 16 : a;
    }
    g_sx = g_widescreen ? kScale * 4.0f / 3.0f : (float)kScale;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MetalArmsPort";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_back_w = kWinW;
    g_back_h = g_widescreen ? kWinW * 9 / 16 : kWinH;
    RECT r = {0, 0, g_back_w, g_back_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Metal Arms: Glitch in the System", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                           wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sc = {};
    sc.BufferCount = 2;
    sc.BufferDesc.Width = g_back_w;
    sc.BufferDesc.Height = g_back_h;
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow = g_hwnd;
    sc.SampleDesc.Count = 1;
    sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sc, &g_swap, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) fail("D3D11CreateDeviceAndSwapChain", hr);
    ID3D11Texture2D *bb;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    g_dev->CreateRenderTargetView(bb, nullptr, &g_back_rtv);
    bb->Release();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)(kEfbW * g_sx); td.Height = kEfbH * kScale; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, nullptr, &g_efb_tex);
    g_dev->CreateRenderTargetView(g_efb_tex, nullptr, &g_efb_rtv);
    g_dev->CreateShaderResourceView(g_efb_tex, nullptr, &g_efb_srv);
    td.Format = DXGI_FORMAT_R32_TYPELESS; // profundidade também lida pelas cópias de Z
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *dt;
    g_dev->CreateTexture2D(&td, nullptr, &dt);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_dev->CreateDepthStencilView(dt, &dd, &g_efb_dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC zd = {};
    zd.Format = DXGI_FORMAT_R32_FLOAT;
    zd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    zd.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(dt, &zd, &g_efb_zsrv);
    dt->Release();

    shader_cache_open();
    ID3DBlob *vs = compile(kVertexShader, "main", "vs_5_0");
    g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    D3D11_INPUT_ELEMENT_DESC il[16] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    for (int i = 0; i < 8; i++)
        il[4 + i] = {"TEXCOORD", (UINT)i, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)(32 + 8 * i), D3D11_INPUT_PER_VERTEX_DATA, 0};
    il[12] = {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 96, D3D11_INPUT_PER_VERTEX_DATA, 0};
    il[13] = {"BLENDINDICES", 1, DXGI_FORMAT_R8G8B8A8_UINT, 0, 100, D3D11_INPUT_PER_VERTEX_DATA, 0};
    il[14] = {"BLENDINDICES", 2, DXGI_FORMAT_R8G8B8A8_UINT, 0, 104, D3D11_INPUT_PER_VERTEX_DATA, 0};
    static_assert(sizeof(GXVertex) == 108, "layout do GXVertex mudou");
    hr = g_dev->CreateInputLayout(il, 15, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout);
    if (FAILED(hr)) fail("CreateInputLayout", hr);
    vs->Release();

    ID3DBlob *bv = compile(kBlitShader, "vs", "vs_5_0"), *bp = compile(kBlitShader, "ps", "ps_5_0");
    g_dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_blit_vs);
    g_dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_blit_ps);
    bv->Release();
    bp->Release();
    bp = compile(kCopyShader, "ps", "ps_5_0");
    g_dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_copy_ps);
    bp->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = g_vb_cap; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_vb);
    bd.ByteWidth = g_ib_cap; bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_ib);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (int b = 0; b < 5; b++) {
        bd.ByteWidth = kXfBlocks[b].words * 4 + (b == 4 ? 64 : 0); // XReg leva a projeção junto
        g_dev->CreateBuffer(&bd, nullptr, &g_xf_cb[b]);
    }
    bd.ByteWidth = (sizeof(PSConst) + 15) & ~15u;
    g_dev->CreateBuffer(&bd, nullptr, &g_ps_cb);
    bd.ByteWidth = 32;
    g_dev->CreateBuffer(&bd, nullptr, &g_blit_cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_dev->CreateSamplerState(&sd, &g_blit_sampler);

    float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_efb_rtv, black);
    g_ctx->ClearDepthStencilView(g_efb_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (getenv("MA_FULLSCREEN")) toggle_borderless(g_hwnd);
    if (getenv("MA_MOUSE")) g_mouse_enabled = atoi(getenv("MA_MOUSE")) != 0;
    if (g_mouse_enabled) {
        RAWINPUTDEVICE rid = {0x01, 0x02, 0, g_hwnd}; // mouse genérico
        RegisterRawInputDevices(&rid, 1, sizeof rid);
    }
    LOG("[gx] Direct3D 11 pronto, EFB %dx%d%s", (int)(kEfbW * g_sx), kEfbH * kScale, g_widescreen ? " (widescreen)" : "");
    return 1;
}

// MA_GX_TRACE=<quadro>: lista cada desenho daquele quadro (diagnóstico)
void trace_draw(int prim, const GXVertex *v, u32 count, u32 vcd_lo, u32 vcd_hi) {
    float m[16];
    build_proj(m);
    u32 pm = v[0].posmtx;
    float p4[4] = {v[0].pos[0], v[0].pos[1], v[0].pos[2], 1}, w[3], c[4];
    for (int r = 0; r < 3; r++) {
        w[r] = 0;
        for (int k = 0; k < 4; k++) w[r] += XFF(pm * 4 + r * 4 + k) * p4[k];
    }
    for (int r = 0; r < 4; r++) c[r] = m[r * 4] * w[0] + m[r * 4 + 1] * w[1] + m[r * 4 + 2] * w[2] + m[r * 4 + 3];
    u32 ns = ((BP(0) >> 10) & 15) + 1;
    char tex[128] = "";
    for (u32 st = 0; st < ns && st < 4; st++) {
        u32 tref = (BP(0x28 + st / 2) >> (12 * (st & 1))) & 0xFFF;
        if ((tref >> 6) & 1) {
            u32 map = tref & 7, i0 = BP(map < 4 ? 0x88 + map : 0xA8 + map - 4);
            char t[32];
            snprintf(t, sizeof t, " m%u:f%u %ux%u", map, (i0 >> 20) & 15, (i0 & 0x3FF) + 1, ((i0 >> 10) & 0x3FF) + 1);
            strncat(tex, t, sizeof tex - strlen(tex) - 1);
        }
    }
    LOG("  draw p%d n=%u %s vp(%.0f,%.0f %.0fx%.0f z %.0f..%.0f) clip(%.2f %.2f %.2f w %.2f) st=%u%s z=%02X cm=%03X ac=%06X ind=%u fog=%u vcd=%05X/%04X",
        prim, count, (XF(0x1026) & 1) ? "orto" : "persp", XFF(0x101D) - XFF(0x101A) - 342, XFF(0x101E) + XFF(0x101B) - 342,
        2 * XFF(0x101A), -2 * XFF(0x101B), XFF(0x101F) - XFF(0x101C), XFF(0x101F), c[0], c[1], c[2], c[3], ns, tex,
        BP(0x40) & 0x1F, BP(0x41) & 0xFFF, BP(0xF3), (BP(0) >> 16) & 7, (BP(0xF1) >> 21) & 7, vcd_lo, vcd_hi);
    if ((BP(0) >> 16) & 7) {
        LOG("     iref=%06X ss=%04X/%04X mtx0=%06X %06X %06X", BP(0x27) & 0xFFFFFF, BP(0x25) & 0xFFFF, BP(0x26) & 0xFFFF,
            BP(0x06) & 0xFFFFFF, BP(0x07) & 0xFFFFFF, BP(0x08) & 0xFFFFFF);
        for (u32 st = 0; st < ns; st++)
            LOG("     tev%u ind=%06X tref=%03X cor=%06X alfa=%06X", st, BP(0x10 + st) & 0x1FFFFF,
                (BP(0x28 + st / 2) >> (12 * (st & 1))) & 0xFFF, BP(0xC0 + 2 * st), BP(0xC1 + 2 * st));
    }
    if (XF(0x1026) & 1) {
        for (u32 st = 0; st < ns; st++)
            LOG("     tev%u cor=%06X alfa=%06X tref=%03X ksel=%03X", st, BP(0xC0 + 2 * st), BP(0xC1 + 2 * st),
                (BP(0x28 + st / 2) >> (12 * (st & 1))) & 0xFFF, (BP(0xF6 + st / 2) >> ((st & 1) ? 14 : 4)) & 0x3FF);
        LOG("     chan c0=%05X a0=%05X mat0=%08X amb0=%08X ncol=%u ntex=%u gm=%06X v0 col=%02X%02X%02X%02X tex=(%.3f %.3f) texgen0=%05X",
            XF(0x100E), XF(0x1010), XF(0x100C), XF(0x100A), XF(0x1009), XF(0x103F), BP(0), v[0].col[0][0], v[0].col[0][1],
            v[0].col[0][2], v[0].col[0][3], v[0].tex[0][0], v[0].tex[0][1], XF(0x1040));
    }
}

extern "C" volatile u64 g_gx_tb, g_gx_draws, g_gx_frames;
volatile u64 g_gx_tb, g_gx_draws, g_gx_frames; // diagnóstico de desempenho

static void gxb_draw_impl(int prim, const GXVertex *v, u32 nverts, const u32 *idx, u32 nidx);

extern "C" void gxb_draw(int prim, const GXVertex *v, u32 nverts, const u32 *idx, u32 nidx) {
    /* medir o tempo de cada desenho custava ~3%: mede 1 a cada 64 e extrapola */
    if ((g_gx_draws++ & 63) == 0) {
        u64 t0 = tb_now();
        gxb_draw_impl(prim, v, nverts, idx, nidx);
        g_gx_tb += (tb_now() - t0) * 64;
    } else {
        gxb_draw_impl(prim, v, nverts, idx, nidx);
    }
}

// grava num buffer dinâmico em anel; devolve o deslocamento em bytes
static u32 ring_upload(ID3D11Buffer *buf, u32 *pos, u32 cap, const void *data, u32 bytes) {
    D3D11_MAPPED_SUBRESOURCE ms;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (*pos + bytes > cap) { *pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    g_ctx->Map(buf, 0, mode, 0, &ms);
    memcpy((u8 *)ms.pData + *pos, data, bytes);
    g_ctx->Unmap(buf, 0);
    u32 off = *pos;
    *pos += (bytes + 15) & ~15u;
    return off;
}

// estados aplicados por último (evita chamadas repetidas ao D3D)
struct {
    ID3D11BlendState *blend; float blend_factor;
    ID3D11DepthStencilState *depth;
    ID3D11RasterizerState *raster;
    ID3D11PixelShader *ps;
    int prim;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[8];
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
} g_last;

// Lote de desenhos com o mesmo estado: vértices e índices acumulam aqui e viram um
// único DrawIndexed quando algum estado muda (ou antes de cópia/apresentação).
std::vector<GXVertex> g_bv;
std::vector<u32> g_bi;
bool g_batch_open; // o estado do lote atual já está aplicado no D3D

static void batch_flush() {
    if (g_bi.empty()) return;
    u32 vbytes = (u32)(g_bv.size() * sizeof(GXVertex)), ibytes = (u32)(g_bi.size() * 4);
    if (vbytes <= g_vb_cap && ibytes <= g_ib_cap) {
        UINT stride = sizeof(GXVertex);
        UINT voff = ring_upload(g_vb, &g_vb_pos, g_vb_cap, g_bv.data(), vbytes);
        UINT ioff = ring_upload(g_ib, &g_ib_pos, g_ib_cap, g_bi.data(), ibytes);
        g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &voff);
        g_ctx->IASetIndexBuffer(g_ib, DXGI_FORMAT_R32_UINT, ioff);
        g_ctx->DrawIndexed((UINT)g_bi.size(), 0, 0);
    }
    g_bv.clear();
    g_bi.clear();
}

static void apply_state(int prim);

/* Widescreen: comprime em x tudo que vai para a tela inteira (a cena ganha campo de visão e o HUD
 * fica em 4:3 no centro), menos os quadros ortográficos que cobrem a largura toda (fades, filtros
 * de tela), que devem esticar. Desenhos em viewport pequeno (mapas de sombra) não mudam. */
static int ws_squeeze(const GXVertex *v, u32 n) {
    if (2 * XFF(0x101A) < 400) return 0; // a cena usa 512x480; mapas de sombra, 128x128
    if (!(XF(0x1026) & 1)) return 1;
    float lo = 1e30f, hi = -1e30f, p0 = XFF(0x1020), p1 = XFF(0x1021);
    for (u32 i = 0; i < n; i++) {
        u32 pm = v[i].posmtx * 4;
        float x = XFF(pm) * v[i].pos[0] + XFF(pm + 1) * v[i].pos[1] + XFF(pm + 2) * v[i].pos[2] + XFF(pm + 3);
        float c = p0 * x + p1;
        if (c < lo) lo = c;
        if (c > hi) hi = c;
    }
    return !(lo <= -0.97f && hi >= 0.97f);
}

static void gxb_draw_impl(int prim, const GXVertex *v, u32 nverts, const u32 *idx, u32 nidx) {
    static long long trace_frame = getenv("MA_GX_TRACE") ? atoll(getenv("MA_GX_TRACE")) : -1;
    if ((long long)g_frame == trace_frame) trace_draw(prim, v, nverts, g_gx.vcd_lo, g_gx.vcd_hi);
    if (prim == 2 && ((BP(0x00) >> 14) & 3) == 3) return; // GX_CULL_ALL

    if (g_widescreen) {
        int sq = ws_squeeze(v, nverts);
        if (sq != g_ws_squeeze) {
            g_ws_squeeze = sq;
            g_xf_dirty |= XFD_REG; // a projeção vai no bloco dos registradores da XF
        }
    }
    static int no_batch = getenv("MA_NO_BATCH") != nullptr; // diagnóstico
    bool same = !no_batch && g_batch_open && !g_force_state && !g_bp_dirty && !g_xf_dirty && prim == g_last.prim;
    if (!same) {
        batch_flush();
        apply_state(prim);
        g_batch_open = true;
        g_bp_dirty = 0;
        g_xf_dirty = 0;
    }
    // MA_BATCH_CHECK: confere se o estado de fato não mudou dentro do lote (diagnóstico)
    static int check = getenv("MA_BATCH_CHECK") != nullptr;
    static u32 snap_bp[0x100], snap_xf[0x1060];
    static s16 snap_reg[4][4];
    static u8 snap_k[4][4];
    if (check) {
        if (!same) {
            memcpy(snap_bp, g_gx.bp, sizeof snap_bp);
            memcpy(snap_xf, g_gx.xfmem, sizeof snap_xf);
            memcpy(snap_reg, g_tev_reg, sizeof snap_reg);
            memcpy(snap_k, g_tev_konst, sizeof snap_k);
        } else {
            static u8 told_bp[0x100], told_xf[0x1060];
            for (int r = 0; r < 0x100; r++)
                if (snap_bp[r] != g_gx.bp[r] && !told_bp[r]++) LOG("[batch] BP %02X mudou sem marcar", r);
            for (int a = 0; a < 0x1060; a++)
                if (snap_xf[a] != g_gx.xfmem[a] && !told_xf[a]++) LOG("[batch] XF %04X mudou sem marcar", a);
            static int told_tev;
            if ((memcmp(snap_reg, g_tev_reg, sizeof snap_reg) || memcmp(snap_k, g_tev_konst, sizeof snap_k)) && !told_tev++)
                LOG("[batch] cores do TEV mudaram sem marcar");
        }
    }
    u32 base = (u32)g_bv.size();
    g_bv.insert(g_bv.end(), v, v + nverts);
    size_t i0 = g_bi.size();
    g_bi.resize(i0 + nidx);
    for (u32 i = 0; i < nidx; i++) g_bi[i0 + i] = idx[i] + base;
    if (g_bv.size() > 32768) batch_flush();
}

/* Avisa uma vez quando o jogo usa um recurso do GX que o renderizador ainda não emula, para que
 * as fases além da primeira digam no log o que falta. */
static void feature_check(void) {
    static u32 seen;
    u32 hit = 0;
    u32 ntex = XF(0x103F) & 15;
    for (u32 i = 0; i < ntex && i < 8; i++)
        if (((XF(0x1040 + i) >> 4) & 7) == 1) hit |= 1; // texgen de emboss
    if (BP(0xF4) & 3) hit |= 2;                          // Z-texture
    u32 cm = BP(0x41);
    if ((cm & 2) && !(cm & 1) && ((cm >> 12) & 15) != 3) hit |= 4; // logic op diferente de cópia
    u32 pix = BP(0x43) & 7;
    if (pix == 1 || pix == 2) hit |= 8; // EFB RGBA6/RGB565 (sem quantização)
    if ((BP(0x00) >> 9) & 1) hit |= 16; // multisample do GX
    hit &= ~seen;
    if (!hit) return;
    seen |= hit;
    static const char *names[] = {"texgen de emboss", "Z-texture", "logic op", "EFB em RGBA6/RGB565", "multisample"};
    for (int i = 0; i < 5; i++)
        if (hit & (1u << i))
            LOG("[gx] recurso ainda não emulado: %s (quadro %llu, aparece só uma vez)", names[i], (unsigned long long)g_frame);
}

static void apply_state(int prim) {
    feature_check();
    if (g_force_state) {
        memset(&g_last, 0, sizeof g_last);
        g_last.prim = -1;
        g_ctx->IASetInputLayout(g_layout);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->VSSetConstantBuffers(0, 5, g_xf_cb);
        g_ctx->PSSetConstantBuffers(0, 1, &g_ps_cb);
        g_ctx->OMSetRenderTargets(1, &g_efb_rtv, g_efb_dsv);
    }
    if (prim != g_last.prim) {
        g_ctx->IASetPrimitiveTopology(prim == 2 ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                      : prim == 5 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                  : D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    }
    upload_constants();

    ID3D11PixelShader *ps = get_ps();
    if (ps != g_last.ps) { g_ctx->PSSetShader(ps, nullptr, 0); g_last.ps = ps; }

    u32 ns = ((BP(0) >> 10) & 15) + 1, used = 0;
    for (u32 st = 0; st < ns; st++) {
        u32 tref = (BP(0x28 + st / 2) >> (12 * (st & 1))) & 0xFFF;
        if ((tref >> 6) & 1) used |= 1u << (tref & 7);
    }
    for (u32 m = 0; m < 8; m++) {
        if (!(used & (1u << m))) continue;
        ID3D11ShaderResourceView *srv = get_texture(m);
        ID3D11SamplerState *smp = get_sampler(m);
        if (srv != g_last.srv[m]) { g_ctx->PSSetShaderResources(m, 1, &srv); g_last.srv[m] = srv; }
        if (smp != g_last.smp[m]) { g_ctx->PSSetSamplers(m, 1, &smp); g_last.smp[m] = smp; }
    }
    float da = (BP(0x42) & 0xFF) / 255.0f;
    ID3D11BlendState *bs = get_blend();
    if (bs != g_last.blend || da != g_last.blend_factor) {
        float bf[4] = {da, da, da, da};
        g_ctx->OMSetBlendState(bs, bf, 0xFFFFFFFF);
        g_last.blend = bs;
        g_last.blend_factor = da;
    }
    ID3D11DepthStencilState *ds = get_depth();
    if (ds != g_last.depth) { g_ctx->OMSetDepthStencilState(ds, 0); g_last.depth = ds; }
    ID3D11RasterizerState *rs = get_raster(prim);
    if (rs != g_last.raster) { g_ctx->RSSetState(rs); g_last.raster = rs; }
    g_last.prim = prim;
    set_viewport_scissor();
    float bf[4] = {da, da, da, da};
    // diagnóstico: MA_GX_DEBUG_ORTHO=máscara desliga estados nos ortográficos
    // (1 blend, 2 Z, 4 culling, 8 scissor)
    static int dbg_ortho = getenv("MA_GX_DEBUG_ORTHO") ? atoi(getenv("MA_GX_DEBUG_ORTHO")) : 0;
    if (dbg_ortho && (XF(0x1026) & 1)) {
        if (dbg_ortho & 1) g_ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
        if (dbg_ortho & 2) g_ctx->OMSetDepthStencilState(nullptr, 0);
        if (dbg_ortho & 4) g_ctx->RSSetState(get_raster(0));
        if (dbg_ortho & 8) {
            D3D11_RECT full = {0, 0, (LONG)(kEfbW * g_sx), kEfbH * kScale};
            g_ctx->RSSetScissorRects(1, &full);
        }
    }
    if (dbg_ortho) g_force_state = 1; // o modo de diagnóstico troca estados por fora do cache
    else g_force_state = 0;
}

static void present(u32 sx, u32 sy, u32 sw, u32 sh) {
    batch_flush();
    g_batch_open = false;
    ID3D11ShaderResourceView *null_srv[8] = {};
    g_ctx->PSSetShaderResources(0, 8, null_srv);
    if (g_resize_w && (g_resize_w != g_back_w || g_resize_h != g_back_h)) { // janela mudou de tamanho
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g_back_rtv->Release();
        g_back_w = g_resize_w;
        g_back_h = g_resize_h;
        g_swap->ResizeBuffers(0, (UINT)g_back_w, (UINT)g_back_h, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D *bb;
        g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
        g_dev->CreateRenderTargetView(bb, nullptr, &g_back_rtv);
        bb->Release();
    }
    g_ctx->OMSetRenderTargets(1, &g_back_rtv, nullptr);
    float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_back_rtv, black);
    /* imagem do console em 4:3 (16:9 no widescreen), centralizada, barras pretas no que sobrar */
    float vw = (float)g_back_w, vh = (float)g_back_h, ax = g_widescreen ? 16.0f : 4.0f, ay = g_widescreen ? 9.0f : 3.0f;
    if (vw * ay > vh * ax) vw = vh * ax / ay; else vh = vw * ay / ax;
    D3D11_VIEWPORT vp = {((float)g_back_w - vw) / 2, ((float)g_back_h - vh) / 2, vw, vh, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT full = {0, 0, g_back_w, g_back_h};
    g_ctx->RSSetScissorRects(1, &full);
    g_ctx->RSSetState(get_raster(0));
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    float rect[4] = {(float)sx / kEfbW, (float)sy / kEfbH, (float)sw / kEfbW, (float)sh / kEfbH};
    D3D11_MAPPED_SUBRESOURCE ms;
    g_ctx->Map(g_blit_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, rect, sizeof rect);
    g_ctx->Unmap(g_blit_cb, 0);
    g_force_state = 1; // a apresentação troca shaders, alvo e estados
    g_ctx->VSSetShader(g_blit_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_blit_cb);
    g_ctx->PSSetShader(g_blit_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_efb_srv);
    g_ctx->PSSetSamplers(0, 1, &g_blit_sampler);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 8, null_srv);

    // captura antes do Present: no modelo flip o back buffer muda depois dele
    static const char *shot_dir = getenv("MA_SHOT_DIR");
    static int every = getenv("MA_SHOT_EVERY") ? atoi(getenv("MA_SHOT_EVERY")) : 60;
    if (shot_dir && every > 0 && (g_frame + 1) % (u64)every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%06llu.png", shot_dir, (unsigned long long)(g_frame + 1));
        screenshot(path);
    }
    u64 tp = tb_now();
    g_swap->Present(g_vsync ? 1 : 0, 0);
    g_gx_tb += tb_now() - tp;
    g_frame++;
    g_gx_frames++;
    static u64 last_tb, last_frame;
    u64 now = tb_now();
    if (now - last_tb > TB_HZ) {
        wchar_t title[160];
        swprintf(title, 160, L"Metal Arms: Glitch in the System  |  %.1f fps  |  %zu shaders, %zu texturas",
                 (double)(g_frame - last_frame) * TB_HZ / (double)(now - last_tb), g_ps_cache.size(), g_tex_cache.size());
        SetWindowTextW(g_hwnd, title);
        last_tb = now;
        last_frame = g_frame;
    }
    gxb_pump_messages();
}

static void efb_copy_convert(ID3D11RenderTargetView *rtv, u32 sx, u32 sy, u32 sw, u32 sh, u32 tw, u32 th, u32 fmt,
                             u32 inten, u32 source) {
    ID3D11ShaderResourceView *null_srv[8] = {};
    g_ctx->PSSetShaderResources(0, 8, null_srv);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)(UINT)(tw * g_sx), (float)(th * kScale), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT full = {0, 0, (LONG)(tw * g_sx), (LONG)(th * kScale)};
    g_ctx->RSSetScissorRects(1, &full);
    g_ctx->RSSetState(get_raster(0));
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    struct { float rect[4]; u32 mode[4]; } cb = {
        {(float)sx / kEfbW, (float)sy / kEfbH, (float)sw / kEfbW, (float)sh / kEfbH}, {fmt, inten, source, 0}};
    D3D11_MAPPED_SUBRESOURCE ms;
    g_ctx->Map(g_blit_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &cb, sizeof cb);
    g_ctx->Unmap(g_blit_cb, 0);
    g_force_state = 1; // a conversão troca shaders, alvo e estados
    g_ctx->VSSetShader(g_blit_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_blit_cb);
    g_ctx->PSSetShader(g_copy_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_blit_cb);
    ID3D11ShaderResourceView *srcs[2] = {g_efb_srv, g_efb_zsrv};
    g_ctx->PSSetShaderResources(0, 2, srcs);
    g_ctx->PSSetSamplers(0, 1, &g_blit_sampler);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 8, null_srv);
    g_ctx->OMSetRenderTargets(1, &g_efb_rtv, g_efb_dsv);
}

extern "C" void gxb_efb_copy(u32 bp52, u32 dest) {
    batch_flush(); /* a cópia precisa do que já foi desenhado */
    u32 tl = BP(0x49), sz = BP(0x4A);
    u32 sx = tl & 0x3FF, sy = (tl >> 10) & 0x3FF, sw = (sz & 0x3FF) + 1, sh = ((sz >> 10) & 0x3FF) + 1;
    static long long trace_frame = getenv("MA_GX_TRACE") ? atoll(getenv("MA_GX_TRACE")) : -1;
    if ((long long)g_frame == trace_frame)
        LOG("  copy %s (%u,%u %ux%u) -> %08X flags %06X", (bp52 & (1u << 14)) ? "XFB" : "textura", sx, sy, sw, sh,
            dest, bp52);
    if (bp52 & (1u << 14)) {
        present(sx, sy, sw, sh);
    } else {
        // cópia para textura: guarda no GPU, associada ao endereço de destino
        EfbCopy &c = g_efb_copies[dest & 0x01FFFFFF];
        u32 half = (bp52 >> 9) & 1, tw = sw >> half, th = sh >> half;
        if (!c.tex || c.w != tw || c.h != th) {
            if (c.rtv) c.rtv->Release();
            if (c.srv) c.srv->Release();
            if (c.tex) c.tex->Release();
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = (UINT)(tw * g_sx); td.Height = th * kScale; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            g_dev->CreateTexture2D(&td, nullptr, &c.tex);
            g_dev->CreateShaderResourceView(c.tex, nullptr, &c.srv);
            g_dev->CreateRenderTargetView(c.tex, nullptr, &c.rtv);
            c.w = tw;
            c.h = th;
        }
        /* formato real: bits 4-6 e bit 3 do campo de formato; bit 15 = intensidade (YUV) */
        u32 fmt = ((bp52 >> 4) & 7) | (((bp52 >> 3) & 1) << 3);
        u32 pix = BP(0x43) & 7; // formato do EFB: 0 RGB8, 1 RGBA6, 2 RGB565, 3 Z24
        u32 inten = ((bp52 >> 15) & 1) && (fmt <= 3 || fmt == 8);
        efb_copy_convert(c.rtv, sx, sy, sw, sh, tw, th, fmt, inten, pix == 3 ? 2 : pix == 1 ? 0 : 1);
        static long long dump_from = getenv("MA_COPY_DUMP") ? atoll(getenv("MA_COPY_DUMP")) : -1;
        static int dumped;
        if (dump_from >= 0 && (long long)g_frame >= dump_from && dumped < 6 && getenv("MA_SHOT_DIR")) {
            dumped++;
            LOG("[gx] cópia %u: (%u,%u %ux%u) -> %08X formato %u intensidade %u EFB %u", dumped, sx, sy, sw, sh, dest, fmt,
                inten, pix);
            char path[512];
            snprintf(path, sizeof path, "%s/copy_%06llu_%08X_f%u.png", getenv("MA_SHOT_DIR"), (unsigned long long)g_frame,
                     dest, fmt);
            save_texture(path, c.tex);
        }
        static const u32 tex_fmt[16] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3, 6, 6, 6}; // formato gravado na RAM
        u32 size = gx_texture_size(tw, th, tex_fmt[fmt]);
        c.mem_hash = hash_bytes(host_ptr(dest), size < 4096 ? size : 4096);
    }
    if (bp52 & (1u << 11)) { // limpar o EFB depois da cópia
        u32 ar = BP(0x4F), gb = BP(0x50);
        float col[4] = {(ar & 0xFF) / 255.0f, ((gb >> 8) & 0xFF) / 255.0f, (gb & 0xFF) / 255.0f, ((ar >> 8) & 0xFF) / 255.0f};
        g_ctx->ClearRenderTargetView(g_efb_rtv, col);
        g_ctx->ClearDepthStencilView(g_efb_dsv, D3D11_CLEAR_DEPTH, (BP(0x51) & 0xFFFFFF) / 16777215.0f, 0);
    }
}

extern "C" void gxb_pump_messages(void) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_closed) {
        gx_print_stats();
        exit(0);
    }
}

extern "C" void gx_frame_pump(void) { gxb_pump_messages(); }

/* Lido pelo PADRead (thread da janela): movimento desde a última leitura e botões.
 * Devolve 0 se o mouse não estiver capturado. */
extern "C" int gx_mouse_poll(int *dx, int *dy, int *left, int *right) {
    if (!g_mouse_captured) return 0;
    *dx = (int)g_mouse_dx;
    *dy = (int)g_mouse_dy;
    g_mouse_dx = g_mouse_dy = 0;
    int lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (g_mouse_swallow) {
        if (lb) lb = 0;
        else g_mouse_swallow = false;
    }
    *left = lb;
    *right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    return 1;
}
