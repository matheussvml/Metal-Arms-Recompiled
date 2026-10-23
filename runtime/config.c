/* Configuração do port: metalarms.ini ao lado do executável.
 *
 * Cada chave vira a variável MA_* correspondente (só se ela ainda não estiver definida no
 * ambiente), então o resto do runtime continua lendo getenv. Também acha a imagem do disco:
 * argumento, chave "disco", procura ao lado do executável ou janela de escolha. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "rt.h"

static char g_ini[MAX_PATH];
static char g_exe_dir[MAX_PATH];
static char g_disc[MAX_PATH];

static const char kDefaultIni[] =
    "; Metal Arms: Glitch in the System - port nativo\n"
    "; Apague uma linha (ou deixe o valor vazio) para usar o padrão.\n"
    "\n"
    "; Imagem do disco (.iso ou .ciso). Vazio = procurar ao lado do executável ou perguntar.\n"
    "disco=\n"
    "\n"
    "; Resolução interna: 1 = original (640x528), 2 = 1280x1056, 3, 4.\n"
    "escala=2\n"
    "\n"
    "; 1 = começar em tela cheia sem bordas (F11 alterna durante o jogo).\n"
    "tela_cheia=0\n"
    "\n"
    "; 1 = espera o monitor ao apresentar o quadro (sem cortes na imagem).\n"
    "vsync=1\n"
    "\n"
    "; Filtro anisotrópico nas texturas: 0 = como no GameCube, 2 a 16 = mais nítido de lado.\n"
    "anisotropia=8\n"
    "\n"
    "; 1 = teclado e mouse são o jogador 1 sozinhos e os controles viram os jogadores 2 a 4.\n"
    "; 0 = o controle 1 e o teclado comandam juntos o jogador 1.\n"
    "teclado_separado=0\n"
    "\n"
    "; Mira com o mouse: clique na janela para capturar; a tecla abaixo do Esc mostra/esconde.\n"
    "; Os botões do mouse ficam na seção [teclado]. 0 = desliga.\n"
    "mouse=1\n"
    "; Sensibilidade: 4 = ~0,14 grau por unidade de movimento do mouse, igual nos dois eixos.\n"
    "; mouse_sensibilidade_vertical multiplica só a vertical.\n"
    "mouse_sensibilidade=4\n"
    "mouse_sensibilidade_vertical=1\n"
    "mouse_inverter_y=0\n"
    "\n"
    "; 1 = imagem em 16:9 (mais campo de visão; o HUD fica na área 4:3 central).\n"
    "widescreen=0\n"
    "\n"
    "; Pasta dos saves do memory card (arquivos .gci).\n"
    "saves=saves/slotA\n"
    "\n"
    "; Pasta com dsp_rom.bin e dsp_coef.bin.\n"
    "dsp_rom=data/dsp\n"
    "\n"
    "; 1 = pula os vídeos (deixa cada um com 1 quadro).\n"
    "pular_videos=0\n"
    "\n"
    "[teclado]\n"
    "; Tecla de cada botão do GameCube; várias separadas por vírgula (ex.: MOUSE_ESQ,E).\n"
    "; Vale letra, dígito, F1-F24, NUM0-NUM9, ENTER, ESC, ESPACO, TAB, SHIFT, CTRL, ALT, BACKSPACE,\n"
    "; SETA_CIMA/BAIXO/ESQUERDA/DIREITA, INSERT, DELETE, HOME, END, PAGEUP, PAGEDOWN, SHIFT_ESQ/DIR,\n"
    "; CTRL_ESQ/DIR, MOUSE_ESQ, MOUSE_DIR, MOUSE_MEIO, MOUSE_4, MOUSE_5 ou NENHUMA.\n"
    "; A tecla abaixo do Esc mostra/esconde o mouse e F11 alterna a tela cheia.\n"
    "; A = pular, B = voltar nos menus, Z = golpe de perto, R = atirar.\n"
    "tecla_a=ESPACO\n"
    "tecla_b=TAB\n"
    "tecla_x=F\n"
    "tecla_y=V\n"
    "tecla_z=MOUSE_DIR\n"
    "tecla_l=Z\n"
    "tecla_r=MOUSE_ESQ,E\n"
    "tecla_start=ESC,ENTER\n"
    "; analógico\n"
    "tecla_cima=W\n"
    "tecla_baixo=S\n"
    "tecla_esquerda=A\n"
    "tecla_direita=D\n"
    "; C-stick (mira; o mouse também mira)\n"
    "tecla_c_cima=I\n"
    "tecla_c_baixo=K\n"
    "tecla_c_esquerda=J\n"
    "tecla_c_direita=L\n"
    "; direcional\n"
    "tecla_dir_cima=SETA_CIMA\n"
    "tecla_dir_baixo=SETA_BAIXO\n"
    "tecla_dir_esquerda=SETA_ESQUERDA\n"
    "tecla_dir_direita=SETA_DIREITA\n";

static const struct {
    const char *key, *env;
} kKeys[] = {
    {"escala", "MA_SCALE"},         {"tela_cheia", "MA_FULLSCREEN"}, {"widescreen", "MA_WIDESCREEN"},
    {"vsync", "MA_VSYNC"},          {"anisotropia", "MA_ANISO"},
    {"mouse", "MA_MOUSE"},          {"mouse_sensibilidade", "MA_MOUSE_SENS"},
    {"mouse_inverter_y", "MA_MOUSE_INVERT"}, {"mouse_sensibilidade_vertical", "MA_MOUSE_SENS_Y"}, {"teclado_separado", "MA_KB_SEPARATE"},
    {"saves", "MA_SAVE_DIR"},       {"dsp_rom", "MA_DSP_ROM"},       {"pular_videos", "MA_SKIP_MOVIES"},
};

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (e - s >= 2 && s[0] == '"' && e[-1] == '"') { e[-1] = 0; s++; }
    return s;
}

/* caminhos relativos do ini valem a partir da pasta dele */
static void resolve(const char *v, char *out, size_t n) {
    if (!v[0] || v[1] == ':' || v[0] == '\\' || v[0] == '/') { snprintf(out, n, "%s", v); return; }
    char base[MAX_PATH];
    snprintf(base, sizeof base, "%s", g_ini);
    char *sl = strrchr(base, '\\');
    if (sl) *sl = 0; else snprintf(base, sizeof base, ".");
    snprintf(out, n, "%s\\%s", base, v);
}

static void apply(const char *key, const char *val) {
    if (!strcmp(key, "disco")) {
        if (val[0]) resolve(val, g_disc, sizeof g_disc);
        return;
    }
    if (!strncmp(key, "tecla_", 6)) { /* teclado: tecla_<botão> -> MA_TECLA_<BOTÃO> */
        char env[64];
        snprintf(env, sizeof env, "MA_TECLA_%s", key + 6);
        for (char *p = env; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (val[0] && !getenv(env)) _putenv_s(env, val);
        return;
    }
    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++) {
        if (strcmp(key, kKeys[i].key)) continue;
        if (getenv(kKeys[i].env) || !val[0]) return; /* o ambiente tem prioridade */
        int is_flag = !strcmp(key, "tela_cheia") || !strcmp(key, "widescreen") || !strcmp(key, "pular_videos") ||
                      !strcmp(key, "mouse_inverter_y") || !strcmp(key, "teclado_separado");
        if (is_flag && atoi(val) == 0) return; /* flags: só a presença conta para quem lê */
        char buf[MAX_PATH];
        if (!strcmp(key, "saves") || !strcmp(key, "dsp_rom")) {
            /* relativo: a primeira que já existir entre a pasta atual, a do ini e uma acima do
             * executável (exe em build/, dados na raiz do projeto); senão, ao lado do ini */
            char up[MAX_PATH];
            snprintf(up, sizeof up, "%s\\..\\%s", g_exe_dir, val);
            if (dir_exists(val)) snprintf(buf, sizeof buf, "%s", val);
            else if (resolve(val, buf, sizeof buf), dir_exists(buf)) {}
            else if (dir_exists(up)) snprintf(buf, sizeof buf, "%s", up);
            if (!strcmp(key, "dsp_rom") && !dir_exists(buf)) return; /* fica a procura padrão */
            val = buf;
        }
        _putenv_s(kKeys[i].env, val);
        return;
    }
    fprintf(stderr, "[config] chave desconhecida: %s\n", key);
}

static void load_ini(void) {
    FILE *f = fopen(g_ini, "rb");
    if (!f) {
        f = fopen(g_ini, "wb");
        if (f) {
            fwrite(kDefaultIni, 1, sizeof kDefaultIni - 1, f);
            fclose(f);
            LOG("[config] criado %s", g_ini);
        }
        f = fopen(g_ini, "rb");
        if (!f) return;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        apply(trim(s), trim(eq + 1));
    }
    fclose(f);
}

/* grava "disco=<caminho>" no ini, trocando a linha existente */
static void save_disc(const char *path) {
    FILE *f = fopen(g_ini, "rb");
    char *old = NULL;
    long n = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        old = malloc(n + 1);
        n = (long)fread(old, 1, n, f);
        old[n] = 0;
        fclose(f);
    }
    f = fopen(g_ini, "wb");
    if (!f) { free(old); return; }
    int written = 0;
    for (char *p = old; p && *p;) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        char *s = p;
        while (*s == ' ' || *s == '\t') s++;
        if (!written && !strncmp(s, "disco", 5) && strchr(s, '=') && strchr(s, '=') < p + len) {
            fprintf(f, "disco=%s\r\n", path);
            written = 1;
        } else {
            fwrite(p, 1, len, f);
        }
        p += len;
    }
    if (!written) fprintf(f, "disco=%s\r\n", path);
    fclose(f);
    free(old);
}

static int find_in(const char *dir, int depth) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        char full[MAX_PATH];
        snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && find_in(full, depth - 1)) { found = 1; break; }
            continue;
        }
        const char *ext = strrchr(fd.cFileName, '.');
        if (!ext || (_stricmp(ext, ".iso") && _stricmp(ext, ".ciso") && _stricmp(ext, ".gcm"))) continue;
        if (!disc_open(full)) continue;
        char id[7] = {0};
        disc_read(0, id, 6);
        disc_close();
        if (!strcmp(id, "GM5E7D")) {
            snprintf(g_disc, sizeof g_disc, "%s", full);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static int ask_disc(void) {
    OPENFILENAMEA ofn = {sizeof ofn};
    char path[MAX_PATH] = "";
    ofn.lpstrFilter = "Imagem do GameCube (*.iso;*.ciso;*.gcm)\0*.iso;*.ciso;*.gcm\0Todos os arquivos\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof path;
    ofn.lpstrTitle = "Escolha a imagem do disco de Metal Arms (GameCube, EUA)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return 0;
    snprintf(g_disc, sizeof g_disc, "%s", path);
    return 1;
}

void config_init(void) {
    GetModuleFileNameA(NULL, g_exe_dir, sizeof g_exe_dir);
    char *sl = strrchr(g_exe_dir, '\\');
    if (sl) *sl = 0;
    snprintf(g_ini, sizeof g_ini, "%s\\metalarms.ini", g_exe_dir);
    load_ini();
    /* a ROM do DSP padrão fica em data/dsp: na pasta atual, na do executável ou uma acima */
    if (!getenv("MA_DSP_ROM") && !dir_exists("data/dsp")) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\data\\dsp", g_exe_dir);
        if (!dir_exists(p)) snprintf(p, sizeof p, "%s\\..\\data\\dsp", g_exe_dir);
        if (dir_exists(p)) _putenv_s("MA_DSP_ROM", p);
    }
}

/* disco: argumento > chave "disco" > procura > janela; lembra o que foi achado */
const char *config_disc(const char *arg) {
    if (arg) return arg;
    if (g_disc[0] && file_exists(g_disc)) return g_disc;
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof cwd, cwd);
    if (find_in(g_exe_dir, 1) || find_in(cwd, 1) || ask_disc()) {
        LOG("[config] disco: %s", g_disc);
        save_disc(g_disc);
        return g_disc;
    }
    return NULL;
}
