/* =============================================================================
 * VortexOS — userspace/vmenu.c
 * vmenu — лаунчер приложений («Start menu» в стиле Kali whisker), отдельный
 * ring3-процесс. Спавнится из vwm по клику на лого V в панели (vpanel шлёт
 * VWM_LAUNCHER_TOGGLE → vwm спавнит /bin/vmenu; повторный клик — close_window).
 *
 * vwm рисует это окно как BORDERLESS POPUP (флаг win->popup): без титлбара,
 * тени-декораций и z-перетаскивания, заякорено под лого V, закрывается по
 * клику мимо окна (как whisker-меню в Kali). Само приложение — обычный
 * libvui-клиент, рисует только содержимое; «безрамочность» обеспечивает vwm.
 *
 * Layout:
 *   ┌────────────┬──────────────────────────────┐
 *   │  Favorites │  [icon] Files                 │
 *   │  Utilities │         Browse the filesystem │
 *   │  System    │  [icon] Terminal              │
 *   │  All       │         Shell and commands    │
 *   │            │  ...                          │
 *   ├────────────┴──────────────────────────────┤
 *   │  VortexOS user                  [ Lock ]   │
 *   └────────────────────────────────────────────┘
 *
 * Поведение:
 *   - клик по категории слева  -> меняет фильтр списка;
 *   - клик по приложению       -> vos_spawn(path) + закрыть лаунчер;
 *   - Lock                     -> vos_spawn("/bin/vlogin") (lock screen) + закрыть;
 *   - Esc / закрытие окна      -> выход.
 *
 * Построен на libvui (immediate-mode), как vsettings/vcalc.
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vui.h>

#define START_W   560
#define START_H   420
#define SIDEBAR_W 150
#define FOOTER_H  46
#define ROW_H     52

/* ----------------------------- каталог приложений --------------------------- */
typedef struct {
    const char *path;
    const char *name;
    const char *desc;
    uint32_t    color;   /* цвет «иконки»-плашки */
} app_t;

static const app_t apps[] = {
    { "/bin/vfiles",    "Files",      "Browse the filesystem",   0xFF4C8BF5 },
    { "/bin/vterm",     "Terminal",   "Shell and commands",      0xFF3CB371 },
    { "/bin/vedit",     "Editor",     "Edit text files",         0xFFE8B64C },
    { "/bin/vcalc",     "Calculator", "Quick calculations",      0xFFB07CE8 },
    { "/bin/vsettings", "Settings",   "System preferences",      0xFFD9534F },
};
#define NAPPS ((int)(sizeof(apps) / sizeof(apps[0])))

/* ------------------------------- категории ---------------------------------- */
/* Битовая маска: какие apps[] входят в категорию (bit i = apps[i]). */
enum { CAT_FAV = 0, CAT_UTIL, CAT_SYS, CAT_ALL, NCATS };
static const char *cat_names[NCATS] = { "Favorites", "Utilities", "System", "All" };
static const unsigned cat_mask[NCATS] = {
    /* FAV  */ (1u << 0) | (1u << 1) | (1u << 3),          /* Files, Terminal, Calculator */
    /* UTIL */ (1u << 2) | (1u << 3) | (1u << 0),          /* Editor, Calculator, Files   */
    /* SYS  */ (1u << 4) | (1u << 1),                      /* Settings, Terminal          */
    /* ALL  */ (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4),
};

static vui_win_t *win;
static int current_cat = CAT_FAV;

/* Отложенное действие: путь для запуска (в render выставляем, в main выполняем
 * и закрываем окно). NULL = ничего. "@LOCK" — специальное действие. */
static const char *pending_launch = 0;

/* ----------------------------- вспомогательное ------------------------------ */
/* Простой скруглённый бэйдж-«иконка»: квадрат с буквой. */
static void draw_icon(int x, int y, int s, uint32_t bg, char letter) {
    vui_rect(win, x, y, s, s, bg);
    vui_frame(win, x, y, s, s, 0x40000000u);
    char l[2] = { letter, 0 };
    int tw = vui_text_width(l);
    vui_text(win, x + (s - tw) / 2, y + (s - VUI_TEXT_H) / 2, l, 0xFFFFFFFF);
}

static void sidebar_item(int y, const char *label, int active) {
    if (active) {
        vui_rect(win, 0, y, SIDEBAR_W, ROW_H - 8, 0xFF2E4A6E);
        vui_rect(win, 0, y, 3, ROW_H - 8, VUI_COL_ACCENT);
    }
    vui_text(win, 16, y + (ROW_H - 8 - VUI_TEXT_H) / 2, label,
             active ? VUI_COL_FG : VUI_COL_DIM);
}

/* ------------------------------- отрисовка ---------------------------------- */
static void render(void) {
    int W = win->w, H = win->h;
    int list_top = 14;
    int footer_y = H - FOOTER_H;

    /* --- сперва прогоняем клики (immediate-mode), чтобы состояние обновилось
     *     ДО отрисовки в этом же кадре (как в vsettings). --- */
    /* категории */
    for (int i = 0; i < NCATS; i++) {
        int y = 60 + i * (ROW_H - 8);
        if (vui_click_in(win, 0, y, SIDEBAR_W, ROW_H - 8)) {
            win->click_pending = 0;
            current_cat = i;
            break;
        }
    }
    /* приложения текущей категории */
    {
        int y = list_top;
        for (int i = 0; i < NAPPS; i++) {
            if (!(cat_mask[current_cat] & (1u << i))) continue;
            if (vui_click_in(win, SIDEBAR_W, y, W - SIDEBAR_W, ROW_H)) {
                win->click_pending = 0;
                pending_launch = apps[i].path;
                break;
            }
            y += ROW_H;
        }
    }
    /* кнопка Lock в футере */
    {
        int bw = 92, bh = 28;
        int bx = W - bw - 14, by = footer_y + (FOOTER_H - bh) / 2;
        if (vui_button(win, bx, by, bw, bh, "Lock", VUI_COL_BTN, VUI_COL_FG))
            pending_launch = "@LOCK";
    }

    /* --- фон --- */
    vui_clear(win, VUI_COL_BG);

    /* --- сайдбар --- */
    vui_rect(win, 0, 0, SIDEBAR_W, H, VUI_COL_PANEL);
    vui_vline(win, SIDEBAR_W - 1, 0, H, 0xFF1A1A24);
    draw_icon(16, 16, 22, VUI_COL_ACCENT, 'V');
    vui_text(win, 46, 16 + (22 - VUI_TEXT_H) / 2, "VortexOS", VUI_COL_FG);
    for (int i = 0; i < NCATS; i++)
        sidebar_item(60 + i * (ROW_H - 8), cat_names[i], current_cat == i);

    /* --- список приложений --- */
    {
        int y = list_top;
        for (int i = 0; i < NAPPS; i++) {
            if (!(cat_mask[current_cat] & (1u << i))) continue;
            int rx = SIDEBAR_W;
            int rw = W - SIDEBAR_W;
            int hot = vui_click_in(win, rx, y, rw, ROW_H);  /* подсветка под курсором клика */
            if (hot) vui_rect(win, rx, y, rw, ROW_H, 0xFF262636);
            draw_icon(rx + 14, y + (ROW_H - 30) / 2, 30, apps[i].color, apps[i].name[0]);
            vui_text(win, rx + 56, y + 9,  apps[i].name, VUI_COL_FG);
            vui_text(win, rx + 56, y + 27, apps[i].desc, VUI_COL_DIM);
            vui_hline(win, rx + 14, y + ROW_H - 1, rw - 28, 0xFF20202C);
            y += ROW_H;
        }
    }

    /* --- футер: профиль + power --- */
    vui_rect(win, SIDEBAR_W, footer_y, W - SIDEBAR_W, FOOTER_H, VUI_COL_PANEL);
    vui_hline(win, SIDEBAR_W, footer_y, W - SIDEBAR_W, 0xFF1A1A24);
    draw_icon(SIDEBAR_W + 14, footer_y + (FOOTER_H - 24) / 2, 24, VUI_COL_OK, 'U');
    vui_text(win, SIDEBAR_W + 48, footer_y + (FOOTER_H - VUI_TEXT_H) / 2,
             "VortexOS user", VUI_COL_FG);
    {
        int bw = 92, bh = 28;
        int bx = win->w - bw - 14, by = footer_y + (FOOTER_H - bh) / 2;
        vui_button(win, bx, by, bw, bh, "Lock", VUI_COL_BTN, VUI_COL_FG);
    }

    vui_flush(win);
}

int main(void) {
    win = vui_open("Vortex Menu", START_W, START_H);
    if (!win) {
        puts("vmenu: failed to create window");
        return 1;
    }
    render();

    vui_event_t ev;
    while (vui_wait_event(win, &ev)) {
        if (ev.type == VUI_EV_KEY && ev.pressed && ev.ch == 27)  /* Esc */
            break;
        if (ev.type == VUI_EV_MOUSE || ev.type == VUI_EV_KEY ||
            ev.type == VUI_EV_RESIZE) {
            render();
            if (pending_launch) {
                const char *path = pending_launch;
                pending_launch = 0;
                if (strcmp(path, "@LOCK") == 0) vos_spawn("/bin/vlogin");
                else                            vos_spawn(path);
                break;                          /* лаунчер закрывается после выбора */
            }
        }
    }
    vui_close(win);
    return 0;
}
