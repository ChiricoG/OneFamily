/*
 * Generic HID Controller Support for OneFamily
 * Based on AnyPad-PS4 (Vld439 / elmonomalvad0)
 *
 * Implements standard USB HID Report Descriptor parsing, dynamic button/axis mapping,
 * override support from /data/onefamily/controllers/VVVV_PPPP.ini, composite secondary keyboard
 * detachment, and force feedback (rumble).
 */

#include "generic_hid.h"
#include "config.h"
#include "log.h"
#include "daemon_syscalls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_CLASS_HID       0x03
#define EP_DIR_IN           0x80
#define EP_XFER_MASK        0x03
#define EP_XFER_INTERRUPT   0x03

enum {
    AX_X, AX_Y, AX_Z, AX_RX, AX_RY, AX_RZ, AX_SLIDER, AX_DIAL,
    AX_BRAKE, AX_ACCEL, AX_HAT, AX_N, AX_NONE = 0xFF
};

enum {
    ROLE_LX, ROLE_LY, ROLE_RX, ROLE_RY, ROLE_L2, ROLE_R2, ROLE_N
};

#define MAX_BUTTONS 32
#define MAX_REPORT_BITS (512 * 8)
#define MAX_RUMBLE_TEMPLATE 16
#define CONTROLLERS_DIR      "/data/onefamily/controllers"
#define CONTROLLERS_DIR_USER "/user/data/onefamily/controllers"

enum {
    RUMBLE_TOKEN_CONST = 0,
    RUMBLE_TOKEN_LARGE,      /* 'L'   : large_motor (0..255) */
    RUMBLE_TOKEN_SMALL,      /* 'R'   : small_motor (0..255) */
    RUMBLE_TOKEN_LARGE_HALF, /* 'l'   : large_motor >> 1 (0..127) */
    RUMBLE_TOKEN_SMALL_HALF, /* 'r'   : small_motor >> 1 (0..127) */
    RUMBLE_TOKEN_LARGE_16LE, /* 'L16' : 2 bytes (large_motor * 257) LE */
    RUMBLE_TOKEN_SMALL_16LE  /* 'R16' : 2 bytes (small_motor * 257) LE */
};

typedef struct {
    uint8_t type;
    uint8_t val;
} RumbleToken;

typedef struct {
    uint8_t present, rid;
    uint16_t off, size;
    int32_t min, max;
} hid_field;

typedef struct {
    int has_ids;
    hid_field axis[AX_N];
    hid_field button[MAX_BUTTONS];      /* usage n at index n - 1 */
    hid_field home, back;               /* consumer page: AC Home, AC Back */
    int nbuttons;
    uint16_t in_report_len;             /* Expected main input report length in bytes (RID 0) */

    uint8_t role[ROLE_N];               /* AX_* per role */
    uint8_t invert[ROLE_N];
    uint32_t button_map[MAX_BUTTONS];

    /* Auto-discovered HID Output Report metadata (Main Item tag == 9) */
    uint8_t  out_present;
    uint8_t  out_has_ids;
    uint8_t  out_rid;
    uint16_t out_bits;
    uint16_t out_data_bit_off;
    uint8_t  out_field_size;

    /* Custom INI rumble override (`rumble = ...`, 0xFF = disabled) */
    uint8_t     custom_rumble_len;
    RumbleToken custom_rumble[MAX_RUMBLE_TEMPLATE];

    /* Active device identifiers */
    uint16_t    vid;
    uint16_t    pid;

    /* HID Application Collection classification */
    uint8_t     is_gamepad;
} GenericHidContext;

typedef struct {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  l2, r2;
} GenericPadState;

typedef struct {
    uint16_t page;
    int32_t lmin, lmax;
    uint32_t size, count;
    uint8_t rid;
} hid_globals;

static GenericHidContext s_ctx;
static GenericPadState   s_state;
static int               s_configured = 0;

/* ---- Descriptor Parsing ---- */

static int32_t item_value(const uint8_t *p, int size, int is_signed) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    if (is_signed && size && size < 4 && (v & (1u << (8 * size - 1))))
        v |= ~0u << (8 * size);
    return (int32_t)v;
}

static void put_field(GenericHidContext *g, uint32_t usage, uint16_t page, const hid_globals *gl, uint16_t off) {
    hid_field f;
    uint16_t pg = usage >> 16 ? (uint16_t)(usage >> 16) : page;
    uint16_t u = (uint16_t)usage;
    hid_field *slot = NULL;

    f.present = 1;
    f.rid = gl->rid;
    f.off = off;
    f.size = (uint16_t)gl->size;
    f.min = gl->lmin;
    f.max = gl->lmax;

    if (pg == 0x01 && u >= 0x30 && u <= 0x37) slot = &g->axis[AX_X + (u - 0x30)];
    else if (pg == 0x01 && u == 0x39) slot = &g->axis[AX_HAT];
    else if (pg == 0x02 && u == 0xC5) slot = &g->axis[AX_BRAKE];
    else if (pg == 0x02 && u == 0xC4) slot = &g->axis[AX_ACCEL];
    else if (pg == 0x09 && u >= 1 && u <= MAX_BUTTONS) {
        slot = &g->button[u - 1];
        if (u > g->nbuttons) g->nbuttons = u;
    }
    else if (pg == 0x0C && u == 0x223) slot = &g->home;
    else if (pg == 0x0C && u == 0x224) slot = &g->back;

    if (slot && !slot->present) *slot = f;
}

static int parse_descriptor(GenericHidContext *g, const uint8_t *d, int n) {
    hid_globals gl, stack[4];
    int depth = 0, i = 0;
    uint32_t usages[64], umin = 0, umax = 0;
    int nusages = 0, has_range = 0;
    uint16_t offset[256];
    uint16_t out_offset[256];

    memset(&gl, 0, sizeof gl);
    memset(offset, 0, sizeof offset);
    memset(out_offset, 0, sizeof out_offset);

    while (i < n) {
        uint8_t p = d[i];
        int size, type, tag;
        const uint8_t *v;
        int32_t val;

        if (p == 0xFE) {
            if (i + 2 >= n) return 0;
            i += 3 + d[i + 1];
            continue;
        }
        size = (p & 3) == 3 ? 4 : (p & 3);
        type = (p >> 2) & 3;
        tag = p >> 4;
        if (i + 1 + size > n) return 0;
        v = d + i + 1;
        val = item_value(v, size, type == 1 && (tag == 1 || tag == 3));
        i += 1 + size;

        if (type == 1) { /* global */
            switch (tag) {
            case 0: gl.page = (uint16_t)val; break;
            case 1: gl.lmin = val; break;
            case 2:
                gl.lmax = gl.lmin >= 0 ? (int32_t)item_value(v, size, 0) : val;
                break;
            case 7: gl.size = (uint32_t)val; break;
            case 8: gl.rid = (uint8_t)val; g->has_ids = 1; break;
            case 9: gl.count = (uint32_t)val; break;
            case 10:
                if (depth < (int)(sizeof stack / sizeof stack[0])) stack[depth++] = gl;
                break;
            case 11:
                if (depth > 0) gl = stack[--depth];
                break;
            }
        } else if (type == 2) { /* local */
            switch (tag) {
            case 0:
                if (nusages < (int)(sizeof usages / sizeof usages[0]))
                    usages[nusages++] = (uint32_t)val;
                break;
            case 1: umin = (uint32_t)val; has_range = 1; break;
            case 2: umax = (uint32_t)val; has_range = 1; break;
            }
        } else if (type == 0) { /* main */
            if (tag == 10) { /* Collection */
                uint16_t page = gl.page;
                uint32_t usage = (nusages > 0) ? usages[0] : 0;
                if (usage >> 16) {
                    page = (uint16_t)(usage >> 16);
                    usage = (uint16_t)usage;
                }
                if (page == 0x01 && (usage == 0x04 || usage == 0x05 || usage == 0x08)) {
                    // Generic Desktop: 4=Joystick, 5=Gamepad, 8=Multi-axis Controller
                    g->is_gamepad = 1;
                } else if (page == 0x02) {
                    // Simulation Controls (Flight Sim, Automobile Wheel/Pedals)
                    g->is_gamepad = 1;
                }
            } else if (tag == 8) { /* input */
                uint16_t *off = &offset[gl.rid];
                uint32_t bits = gl.size * gl.count;
                if (*off + bits > MAX_REPORT_BITS) return 0;
                if (!(val & 1)) { /* data, not constant padding */
                    for (uint32_t k = 0; k < gl.count; k++) {
                        uint32_t u;
                        if (nusages) u = usages[k < (uint32_t)nusages ? k : (uint32_t)nusages - 1];
                        else if (has_range) u = umin + k > umax ? umax : umin + k;
                        else break;
                        put_field(g, u, gl.page, &gl, (uint16_t)(*off + k * gl.size));
                    }
                }
                *off = (uint16_t)(*off + bits);
                if (gl.rid == 0) {
                    g->in_report_len = (uint16_t)((*off + 7) / 8);
                }
            } else if (tag == 9) { /* output (force-feedback / rumble / LEDs) */
                uint16_t *off = &out_offset[gl.rid];
                uint32_t bits = gl.size * gl.count;
                if (*off + bits <= MAX_REPORT_BITS && bits > 0) {
                    if (!g->out_present) {
                        g->out_present = 1;
                        g->out_has_ids = (gl.rid != 0);
                        g->out_rid = gl.rid;
                        g->out_data_bit_off = *off;
                        g->out_field_size = (gl.size == 16) ? 16 : 8;
                    } else if (g->out_rid == gl.rid && !(val & 1)) {
                        if (gl.size == 16) {
                            g->out_field_size = 16;
                        }
                    }
                    *off = (uint16_t)(*off + bits);
                    if (g->out_rid == gl.rid) {
                        g->out_bits = *off;
                    }
                }
            }
            nusages = 0;
            has_range = 0;
            umin = umax = 0;
        }
    }
    return 1;
}

/* ---- Mapping Engine ---- */

static const struct { const char *name; uint32_t bit; } k_buttons[] = {
    { "cross", DS4_BUTTON_CROSS }, { "circle", DS4_BUTTON_CIRCLE }, { "square", DS4_BUTTON_SQUARE },
    { "triangle", DS4_BUTTON_TRIANGLE }, { "l1", DS4_BUTTON_L1 }, { "r1", DS4_BUTTON_R1 },
    { "l2", DS4_BUTTON_L2 }, { "r2", DS4_BUTTON_R2 }, { "l3", DS4_BUTTON_L3 }, { "r3", DS4_BUTTON_R3 },
    { "create", DS4_BUTTON_SHARE }, { "share", DS4_BUTTON_SHARE }, { "options", DS4_BUTTON_OPTIONS },
    { "ps", DS4_BUTTON_PS }, { "touchpad", DS4_BUTTON_TOUCHPAD }, { "up", DS4_BUTTON_DPAD_UP },
    { "down", DS4_BUTTON_DPAD_DOWN }, { "left", DS4_BUTTON_DPAD_LEFT }, { "right", DS4_BUTTON_DPAD_RIGHT },
    { "none", 0 },
};

static const char *const k_axes[AX_N] = {
    "x", "y", "z", "rx", "ry", "rz", "slider", "dial", "brake", "accel", "hat",
};

static const char *const k_roles[ROLE_N] = { "lx", "ly", "rx", "ry", "l2", "r2" };

static void default_mapping(GenericHidContext *g) {
    static const uint32_t android[15] = {
        DS4_BUTTON_CROSS, DS4_BUTTON_CIRCLE, 0, DS4_BUTTON_SQUARE, DS4_BUTTON_TRIANGLE, 0, DS4_BUTTON_L1, DS4_BUTTON_R1,
        DS4_BUTTON_L2, DS4_BUTTON_R2, DS4_BUTTON_SHARE, DS4_BUTTON_OPTIONS, DS4_BUTTON_PS, DS4_BUTTON_L3, DS4_BUTTON_R3,
    };
    static const uint32_t dinput[14] = {
        DS4_BUTTON_SQUARE, DS4_BUTTON_CROSS, DS4_BUTTON_CIRCLE, DS4_BUTTON_TRIANGLE, DS4_BUTTON_L1, DS4_BUTTON_R1,
        DS4_BUTTON_L2, DS4_BUTTON_R2, DS4_BUTTON_SHARE, DS4_BUTTON_OPTIONS, DS4_BUTTON_L3, DS4_BUTTON_R3, 0, 0,
    };
    const hid_field *a = g->axis;

    memset(g->button_map, 0, sizeof g->button_map);
    if (g->nbuttons >= 15) {
        for (int i = 0; i < 15; i++) g->button_map[i] = android[i];
    } else {
        for (int i = 0; i < 14; i++) g->button_map[i] = dinput[i];
    }

    memset(g->role, AX_NONE, sizeof g->role);
    memset(g->invert, 0, sizeof g->invert);
    if (a[AX_X].present) g->role[ROLE_LX] = AX_X;
    if (a[AX_Y].present) g->role[ROLE_LY] = AX_Y;

    if (a[AX_Z].present && a[AX_RZ].present) {
        g->role[ROLE_RX] = AX_Z;
        g->role[ROLE_RY] = AX_RZ;
        if (a[AX_RX].present && a[AX_RY].present && !a[AX_BRAKE].present) {
            g->role[ROLE_L2] = AX_RX;
            g->role[ROLE_R2] = AX_RY;
        }
    } else if (a[AX_RX].present && a[AX_RY].present) {
        g->role[ROLE_RX] = AX_RX;
        g->role[ROLE_RY] = AX_RY;
        if (a[AX_Z].present && !a[AX_BRAKE].present) g->role[ROLE_L2] = AX_Z;
        if (a[AX_RZ].present && !a[AX_ACCEL].present) g->role[ROLE_R2] = AX_RZ;
    }
    if (a[AX_BRAKE].present) g->role[ROLE_L2] = AX_BRAKE;
    if (a[AX_ACCEL].present) g->role[ROLE_R2] = AX_ACCEL;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    return s;
}

static int iequals(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int parse_ini_line(GenericHidContext *g, char *line) {
    line = trim(line);
    if (!*line || *line == '#' || *line == ';') return 1;
    // Skip section headers like [Info], [Buttons], [Axes], [Invert]
    if (*line == '[') return 1;

    // Check for legacy "invert <role>"
    if (strncmp(line, "invert ", 7) == 0) {
        char *val = trim(line + 7);
        for (size_t i = 0; i < ROLE_N; i++) {
            if (iequals(val, k_roles[i])) { g->invert[i] = 1; return 1; }
        }
        return 0;
    }

    // Split key = val
    char *eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = '\0';
    char *key = trim(line);
    char *val = trim(eq + 1);

    // Strip inline comments from val (anything after '#' or ';')
    char *c = val;
    while (*c) {
        if (*c == '#' || *c == ';') {
            *c = '\0';
            break;
        }
        c++;
    }
    val = trim(val);
    if (!*val) return 1;

    // 1. buttonN = target
    unsigned n = 0;
    if (sscanf(key, "button%u", &n) == 1 && n >= 1 && n <= MAX_BUTTONS) {
        for (size_t i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++) {
            if (iequals(val, k_buttons[i].name)) {
                g->button_map[n - 1] = k_buttons[i].bit;
                return 1;
            }
        }
        return 0;
    }

    // 2. invert_role = 1/0/true/false
    if (strncmp(key, "invert_", 7) == 0 || strncmp(key, "INVERT_", 7) == 0) {
        char *role_name = key + 7;
        int inv = (strcmp(val, "1") == 0 || iequals(val, "true") || iequals(val, "yes"));
        for (size_t i = 0; i < ROLE_N; i++) {
            if (iequals(role_name, k_roles[i])) {
                g->invert[i] = (uint8_t)inv;
                return 1;
            }
        }
        return 0;
    }

    // 3. role = axis_source (e.g. lx = x, l2 = rx)
    for (size_t i = 0; i < ROLE_N; i++) {
        if (!iequals(key, k_roles[i])) continue;
        if (iequals(val, "none")) { g->role[i] = AX_NONE; return 1; }
        for (size_t a = 0; a < AX_N; a++) {
            if (iequals(val, k_axes[a])) { g->role[i] = (uint8_t)a; return 1; }
        }
        return 0;
    }

    // 4. rumble = preset or hex template (e.g. "rumble = 05 L R 00 00")
    if (iequals(key, "rumble") || iequals(key, "rumble_report")) {
        if (iequals(val, "none") || iequals(val, "off") || strcmp(val, "0") == 0) {
            g->custom_rumble_len = 0xFF;
            return 1;
        }
        if (iequals(val, "auto") || iequals(val, "default")) {
            g->custom_rumble_len = 0;
            return 1;
        }

        char tpl_buf[128];
        if (iequals(val, "logitech")) {
            snprintf(tpl_buf, sizeof(tpl_buf), "51 00 L 00 R 00 00");
        } else if (iequals(val, "dragonrise")) {
            snprintf(tpl_buf, sizeof(tpl_buf), "FA FE L R 00 00 00");
        } else if (iequals(val, "greenasia")) {
            snprintf(tpl_buf, sizeof(tpl_buf), "51 00 r l");
        } else if (iequals(val, "betop") || iequals(val, "shanwan")) {
            snprintf(tpl_buf, sizeof(tpl_buf), "00 00 L R");
        } else if (iequals(val, "8bitdo")) {
            snprintf(tpl_buf, sizeof(tpl_buf), "81 11 04 08 00 04 L R");
        } else if (iequals(val, "dinput05")) {
            snprintf(tpl_buf, sizeof(tpl_buf), "05 L R 00 00");
        } else {
            snprintf(tpl_buf, sizeof(tpl_buf), "%s", val);
        }

        g->custom_rumble_len = 0;
        char *p = tpl_buf;
        while (*p && g->custom_rumble_len < MAX_RUMBLE_TEMPLATE) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            char *tok = p;
            while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
            if (*p) *p++ = '\0';

            RumbleToken rt = { RUMBLE_TOKEN_CONST, 0 };
            if (strcmp(tok, "L") == 0) {
                rt.type = RUMBLE_TOKEN_LARGE;
            } else if (strcmp(tok, "R") == 0) {
                rt.type = RUMBLE_TOKEN_SMALL;
            } else if (strcmp(tok, "l") == 0) {
                rt.type = RUMBLE_TOKEN_LARGE_HALF;
            } else if (strcmp(tok, "r") == 0) {
                rt.type = RUMBLE_TOKEN_SMALL_HALF;
            } else if (iequals(tok, "L16")) {
                rt.type = RUMBLE_TOKEN_LARGE_16LE;
            } else if (iequals(tok, "R16")) {
                rt.type = RUMBLE_TOKEN_SMALL_16LE;
            } else {
                rt.type = RUMBLE_TOKEN_CONST;
                rt.val = (uint8_t)strtoul(tok, NULL, 16);
            }
            g->custom_rumble[g->custom_rumble_len++] = rt;
        }
        return 1;
    }

    return 0;
}

#define ANDROID_ORDER \
    "button1=cross\nbutton2=circle\nbutton3=none\nbutton4=square\nbutton5=triangle\n" \
    "button6=none\nbutton7=l1\nbutton8=r1\nbutton9=l2\nbutton10=r2\nbutton11=create\n" \
    "button12=options\nbutton13=ps\nbutton14=l3\nbutton15=r3\n"

#define LOGITECH_DINPUT_ORDER \
    "button1=square\nbutton2=cross\nbutton3=circle\nbutton4=triangle\n" \
    "button5=l1\nbutton6=r1\nbutton7=l2\nbutton8=r2\n" \
    "button9=share\nbutton10=options\nbutton11=l3\nbutton12=r3\n" \
    "lx=x\nly=y\nrx=z\nry=rz\n"

#define SHANWAN_ORDER \
    "button1=square\nbutton2=cross\nbutton3=circle\nbutton4=triangle\n" \
    "button5=l1\nbutton6=r1\nbutton7=l2\nbutton8=r2\n" \
    "button9=create\nbutton10=options\nbutton11=l3\nbutton12=r3\n" \
    "button13=none\nbutton14=none\n" \
    "lx=x\nly=y\nrx=z\nry=rz\n" \
    "rumble=none\n"

static const struct { uint16_t vid, pid; const char *map; } k_builtin[] = {
    { 0x2563, 0x0119, SHANWAN_ORDER }, /* ShanWan 2.4G Wireless Dongle / BSP-G6 */
    { 0x2563, 0x0523, SHANWAN_ORDER }, /* ShanWan PS3/PC Gamepad */
    { 0x2563, 0x0575, SHANWAN_ORDER }, /* ShanWan Wireless Gamepad */
    { 0x046D, 0xC216, LOGITECH_DINPUT_ORDER }, /* Logitech F310 / Dual Action (DirectInput mode) */
    { 0x046D, 0xC219, LOGITECH_DINPUT_ORDER }, /* Logitech F710 Wireless / Cordless Rumblepad 2 (DirectInput mode) */
    { 0x046D, 0xC218, LOGITECH_DINPUT_ORDER }, /* Logitech RumblePad 2 / F510 (DirectInput mode) */
    { 0x046D, 0xC211, LOGITECH_DINPUT_ORDER }, /* Logitech WingMan RumblePad (DirectInput mode) */
    { 0xFFFF, 0x046E, ANDROID_ORDER },      /* GameSir G3s */
    { 0x05AC, 0x022D, ANDROID_ORDER },      /* GameSir G3s, alternate mode */
    { 0xFFFF, 0x046F, ANDROID_ORDER },      /* GameSir G4s */
    { 0x3537, 0x1022, ANDROID_ORDER },      /* GameSir G7 Pro */
    { 0xFFFF, 0x0450, ANDROID_ORDER },      /* GameSir T1s */
    { 0x05AC, 0x056B, ANDROID_ORDER },      /* GameSir T2a */
    { 0x20BC, 0x5501, ANDROID_ORDER },      /* Betop 2585N2 */
    { 0x2E2C, 0x0002, ANDROID_ORDER },      /* Bionik Vulkan */
    { 0x0079, 0x181C, ANDROID_ORDER },      /* LanShen X1Pro */
    { 0x2717, 0x3144, ANDROID_ORDER },      /* Xiaomi Mi Controller */
    { 0x1949, 0x0402, ANDROID_ORDER },      /* Amazon Fire / iPega */
    { 0x2DC8, 0x2100, ANDROID_ORDER },      /* 8BitDo SN30 Pro for Xbox Cloud */
    { 0x2DC8, 0x2101, ANDROID_ORDER },
    { 0x2DC8, 0x3012, ANDROID_ORDER },      /* 8BitDo Ultimate 2.4g */
    { 0x2DC8, 0x310B, ANDROID_ORDER },      /* 8BitDo Ultimate 2 Wireless */
    { 0x2DC8, 0x310A, ANDROID_ORDER },      /* 8BitDo Ultimate Wireless receiver */
    { 0x2DC8, 0x6012, ANDROID_ORDER },      /* 8BitDo Ultimate 2C Wireless */
    { 0x1949, 0x0403, ANDROID_ORDER "button3=l3\nbutton6=r3\nbutton13=none\n" },
    { 0x05AC, 0x022C, ANDROID_ORDER "button3=l3\nbutton6=r3\nbutton13=none\n" },
    { 0x1038, 0x1412, ANDROID_ORDER "button11=none\nbutton12=options\nbutton13=create\n" },
    { 0x0111, 0x1420, ANDROID_ORDER "button11=none\nbutton12=options\nbutton13=create\n" },
    { 0x0111, 0x1431, ANDROID_ORDER "button11=none\nbutton12=options\nbutton13=create\n" },
    { 0x0111, 0x1419, ANDROID_ORDER "button11=none\nbutton12=options\nbutton13=create\n" },
    { 0x0955, 0x7214, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton7=l3\nbutton8=r3\nbutton9=options\n" },
    { 0x1532, 0x0900, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton7=create\nbutton8=options\nbutton9=l3\n"
                      "button10=r3\nbutton11=none\nbutton12=ps\nbutton13=create\n" },
    { 0x20D6, 0x89E5, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton7=create\nbutton8=options\nbutton9=l3\nbutton10=r3\n" },
    { 0x20D6, 0x0DAD, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton7=create\nbutton8=options\nbutton9=l3\nbutton10=r3\n" },
    { 0x20D6, 0x6271, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton7=create\nbutton8=options\nbutton9=l3\nbutton10=r3\n" },
    { 0x3250, 0x1002, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton7=l3\nbutton8=r3\nbutton9=create\n"
                      "button10=options\nbutton11=ps\n" },
    { 0x2E24, 0x200A, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton9=create\nbutton10=options\n" },
    { 0x04E8, 0x046E, "button1=cross\nbutton2=circle\nbutton3=square\nbutton4=triangle\n"
                      "button5=l1\nbutton6=r1\nbutton7=create\nbutton8=options\nbutton9=l3\n"
                      "button10=r3\nbutton11=l2\nbutton12=r2\n" },
};

static int has_builtin_map(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < sizeof k_builtin / sizeof k_builtin[0]; i++) {
        if (k_builtin[i].vid == vid && k_builtin[i].pid == pid) return 1;
    }
    return 0;
}

static int builtin_map(GenericHidContext *g, uint16_t vid, uint16_t pid) {
    char buf[1024];
    for (size_t i = 0; i < sizeof k_builtin / sizeof k_builtin[0]; i++) {
        if (k_builtin[i].vid != vid || k_builtin[i].pid != pid) continue;
        snprintf(buf, sizeof buf, "%s", k_builtin[i].map);
        char *line = buf;
        while (line && *line) {
            char *next = strchr(line, '\n');
            if (next) *next++ = '\0';
            parse_ini_line(g, line);
            line = next;
        }
        daemon_log(LOG_LEVEL_INFO, "Generic HID: applied builtin map for 0x%04X:0x%04X", vid, pid);
        return 1;
    }
    return 0;
}

static void load_controller_ini(GenericHidContext *g, uint16_t vid, uint16_t pid, int has_builtin) {
    char path[128];
    snprintf(path, sizeof path, "%s/%04X_%04X.ini", CONTROLLERS_DIR, vid, pid);
    int fd = daemon_sys_open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        snprintf(path, sizeof path, "%s/%04X_%04X.ini", CONTROLLERS_DIR_USER, vid, pid);
        fd = daemon_sys_open(path, 0, 0);
    }
    if (fd < 0) {
        snprintf(path, sizeof path, "%s/%04x_%04x.ini", CONTROLLERS_DIR, vid, pid);
        fd = daemon_sys_open(path, 0, 0);
    }
    if (fd < 0) {
        snprintf(path, sizeof path, "%s/%04x_%04x.ini", CONTROLLERS_DIR_USER, vid, pid);
        fd = daemon_sys_open(path, 0, 0);
    }
    if (fd < 0 && !has_builtin) {
        snprintf(path, sizeof path, "%s/default.ini", CONTROLLERS_DIR);
        fd = daemon_sys_open(path, 0, 0);
        if (fd < 0) {
            snprintf(path, sizeof path, "%s/default.ini", CONTROLLERS_DIR_USER);
            fd = daemon_sys_open(path, 0, 0);
        }
    }
    if (fd < 0) return;

    static char buf[4096];
    memset(buf, 0, sizeof(buf));
    int bytes_read = daemon_sys_read(fd, buf, sizeof(buf) - 1);
    daemon_sys_close(fd);

    if (bytes_read <= 0) return;

    char *line = buf;
    char *next_line = NULL;
    int line_no = 0;
    while (line && *line) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        line_no++;
        parse_ini_line(g, line);
        line = next_line;
    }
    daemon_log(LOG_LEVEL_INFO, "Generic HID: loaded controller INI %s (%d lines)", path, line_no);
}

/* ---- Input Reading ---- */

static int32_t get_bits(const uint8_t *d, int len, const hid_field *f) {
    uint32_t v = 0;
    if ((f->off + f->size + 7) / 8 > len || f->size == 0 || f->size > 32) return f->min;
    for (int i = 0; i < f->size; i++) {
        int bit = f->off + i;
        if (d[bit >> 3] & (1 << (bit & 7))) v |= 1u << i;
    }
    if (f->min < 0 && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size;
    return (int32_t)v;
}

static uint8_t to_byte(const hid_field *f, int32_t v) {
    int64_t span = (int64_t)f->max - f->min;
    if (span <= 0) return 128;
    int64_t s = ((int64_t)v - f->min) * 255 / span;
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

/* ---- Public API ---- */

int generic_hid_detect(libusb_device* dev, DetectedController* out) {
    if (!dev || !out) return 0;

    struct libusb_config_descriptor* cfg = NULL;
    if (usbd_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        return 0;
    }

    uint8_t kbd_iface = 0xFF;
    int gamepad_found = 0;
    uint8_t gp_iface = 0;
    uint8_t gp_alt = 0;
    uint8_t gp_ep_in = 0;
    uint8_t gp_ep_out = 0;
    uint16_t gp_mps = 0;
    uint8_t gp_interval = 0;

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface* intf = &cfg->interface[i];
        for (int a = 0; a < intf->num_altsetting; a++) {
            const struct libusb_interface_descriptor* alt = &intf->altsetting[a];

            // Detect boot keyboard interface (e.g. 8BitDo macro / configuration keyboard)
            if (alt->bInterfaceClass == USB_CLASS_HID && alt->bInterfaceSubClass == 1 && alt->bInterfaceProtocol == 1) {
                kbd_iface = alt->bInterfaceNumber;
                daemon_log(LOG_LEVEL_INFO, "Generic HID: detected composite keyboard interface %d", kbd_iface);
                continue;
            }

            // Skip boot mouse
            if (alt->bInterfaceClass == USB_CLASS_HID && alt->bInterfaceSubClass == 1 && alt->bInterfaceProtocol == 2) {
                continue;
            }

            // Check HID Gamepad interface (Class 0x03)
            if (alt->bInterfaceClass == USB_CLASS_HID && !gamepad_found) {
                uint8_t ep_in = 0;
                uint8_t ep_out = 0;
                uint16_t ep_in_mps = 0;
                uint8_t ep_in_interval = 0;
                for (int k = 0; k < alt->bNumEndpoints; k++) {
                    const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                    if ((ep->bmAttributes & EP_XFER_MASK) == EP_XFER_INTERRUPT) {
                        if (ep->bEndpointAddress & EP_DIR_IN) {
                            if (ep_in == 0) {
                                ep_in = ep->bEndpointAddress;
                                ep_in_mps = ep->wMaxPacketSize;
                                ep_in_interval = ep->bInterval;
                            }
                        } else {
                            if (ep_out == 0) ep_out = ep->bEndpointAddress;
                        }
                    }
                }
                if (ep_in != 0) {
                    gp_iface = alt->bInterfaceNumber;
                    gp_alt = alt->bAlternateSetting;
                    gp_ep_in = ep_in;
                    gp_ep_out = ep_out;
                    gp_mps = ep_in_mps;
                    gp_interval = ep_in_interval;
                    gamepad_found = 1;
                }
            }
        }
    }

    usbd_free_config_descriptor(cfg);

    if (gamepad_found) {
        out->type = CONTROLLER_GENERIC_HID;
        out->interface_num = gp_iface;
        out->alt_setting = gp_alt;
        out->ep_in = gp_ep_in;
        out->ep_out = gp_ep_out;
        out->kbd_interface_num = kbd_iface;
        out->wMaxPacketSize = gp_mps;
        out->bInterval = gp_interval;
        return 1;
    }

    return 0;
}

int generic_hid_setup(libusb_device_handle* h, uint16_t vid, uint16_t pid, uint8_t iface,
                      int (*ctrl_transfer)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, uint32_t)) {
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(&s_state, 0, sizeof(s_state));
    s_state.lx = s_state.ly = s_state.rx = s_state.ry = 128;
    s_configured = 0;
    s_ctx.vid = vid;
    s_ctx.pid = pid;

    // Check for builtin profile first: if device has a known layout, avoid crashing EP0
    int has_builtin = has_builtin_map(vid, pid);
    if (has_builtin) {
        s_ctx.is_gamepad = 1;
    }

    static uint8_t report_desc[256];
    int desc_len = 0;
    bool is_legacy_hid = (vid == 0x2563 || vid == 0x20BC);

    // Only query report descriptor for unmapped controllers that are not legacy/fragile chipsets
    if (ctrl_transfer && !has_builtin && !is_legacy_hid) {
        // GET_DESCRIPTOR: type 0x22 (HID Report Descriptor), bmRequestType 0x81 (Standard, In, Interface)
        desc_len = ctrl_transfer(h, 0x81, 0x06, 0x2200, (uint16_t)iface, report_desc, sizeof(report_desc), 500);
        if (desc_len <= 0) {
            // Fallback: bmRequestType 0x80 (Standard, In, Device), wIndex = 0
            desc_len = ctrl_transfer(h, 0x80, 0x06, 0x2200, 0, report_desc, sizeof(report_desc), 500);
        }
        // Universal HID Wake-up: Send SET_IDLE (bmRequestType: 0x21, bRequest: 0x0A, wValue: 0, wIndex: iface)
        // Many microcontroller pads/dongles stay in low-power idle until a SET_IDLE arrives.
        // Skip SET_IDLE for ShanWan, Betop and Logitech (0x2563, 0x20BC, 0x046D):
        // Logitech RumblePad 2 / Cordless / Dual Action suffer from the LG_NOGET hardware quirk
        // where control requests stall the USB pipe and silence the interrupt endpoint.
        if (vid != 0x046D) {
            ctrl_transfer(h, 0x21, 0x0A, 0x0000, (uint16_t)iface, NULL, 0, 100);
        }
    }

    if (desc_len > 0) {
        if (!parse_descriptor(&s_ctx, report_desc, desc_len)) {
            daemon_log(LOG_LEVEL_WARN, "Generic HID (0x%04X:0x%04X): malformed report descriptor (%d bytes)", vid, pid, desc_len);
        } else {
            daemon_log(LOG_LEVEL_INFO, "Generic HID (0x%04X:0x%04X): parsed %d buttons, axes: X=%d Y=%d Z=%d RX=%d RY=%d RZ=%d HAT=%d",
                       vid, pid, s_ctx.nbuttons,
                       s_ctx.axis[AX_X].present, s_ctx.axis[AX_Y].present, s_ctx.axis[AX_Z].present,
                       s_ctx.axis[AX_RX].present, s_ctx.axis[AX_RY].present, s_ctx.axis[AX_RZ].present,
                       s_ctx.axis[AX_HAT].present);
            if (s_ctx.out_present) {
                daemon_log(LOG_LEVEL_INFO, "Generic HID (0x%04X:0x%04X): discovered Output Report (rid=0x%02X, has_id=%d, bits=%u, data_off=%u, field_bits=%u)",
                           vid, pid, s_ctx.out_rid, s_ctx.out_has_ids, s_ctx.out_bits, s_ctx.out_data_bit_off, s_ctx.out_field_size);
            }
            s_configured = 1;

            int has_axes = 0;
            for (int a = 0; a < AX_N; a++) {
                if (s_ctx.axis[a].present) { has_axes = 1; break; }
            }
            if (s_ctx.nbuttons == 0 && !has_axes) {
                daemon_log(LOG_LEVEL_WARN, "Generic HID (0x%04X:0x%04X): device has 0 buttons and 0 axes (idle receiver/non-gamepad) — rejecting claim", vid, pid);
                return 0;
            }
        }
    }

    if (desc_len <= 0) {
        if (!has_builtin) {
            daemon_log(LOG_LEVEL_WARN, "Generic HID (0x%04X:0x%04X): could not read report descriptor (ret=%d) — rejecting non-gamepad claim",
                       vid, pid, desc_len);
            return 0;
        }
        if (!is_legacy_hid) {
            daemon_log(LOG_LEVEL_WARN, "Generic HID (0x%04X:0x%04X): could not read report descriptor (ret=%d), using fallback mapping for known controller",
                       vid, pid, desc_len);
        }
    } else {
        bool has_directional_control = (s_ctx.axis[AX_X].present && s_ctx.axis[AX_Y].present) || s_ctx.axis[AX_HAT].present;
        if (!s_ctx.is_gamepad && !has_builtin && !has_directional_control) {
            daemon_log(LOG_LEVEL_INFO, "Generic HID (0x%04X:0x%04X): non-gamepad HID device (no Gamepad/Joystick collection or directional axes) — rejecting claim",
                       vid, pid);
            return 0;
        }
    }

    default_mapping(&s_ctx);
    has_builtin = builtin_map(&s_ctx, vid, pid);
    load_controller_ini(&s_ctx, vid, pid, has_builtin);

    return 1;
}

void generic_hid_reset(void) {
    memset(&s_state, 0, sizeof(s_state));
    s_state.lx = s_state.ly = s_state.rx = s_state.ry = 128;
}

int generic_hid_get_nbuttons(void) {
    return s_ctx.nbuttons > 0 ? s_ctx.nbuttons : 16;
}

void generic_hid_reload(uint16_t vid, uint16_t pid) {
    s_ctx.vid = vid;
    s_ctx.pid = pid;
    s_ctx.custom_rumble_len = 0;
    default_mapping(&s_ctx);
    int has_builtin = builtin_map(&s_ctx, vid, pid);
    load_controller_ini(&s_ctx, vid, pid, has_builtin);
    daemon_log(LOG_LEVEL_INFO, "Generic HID: reloaded mapping for 0x%04X:0x%04X", vid, pid);
}

int generic_hid_parse_report(const uint8_t* rx, int len, OrbisPadData* out, const TranslatorConfig* cfg) {
    if (!rx || len <= 0 || !out) return 0;

    // Discard stale packets from another controller mode (e.g. 20-byte Xbox packet on an 8-byte DirectInput controller)
    if (s_ctx.in_report_len > 0 && !s_ctx.has_ids && len > s_ctx.in_report_len + 4) {
        return 0;
    }

    static const uint32_t dirs[8] = {
        DS4_BUTTON_DPAD_UP,
        DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_DOWN,
        DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_LEFT,
        DS4_BUTTON_DPAD_LEFT,
        DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_LEFT,
    };

    // Dedicated deterministic parsing for Logitech DirectInput gamepads
    // (VID 0x046D: RumblePad 2 USB 0xC218, Cordless 0xC219, Dual Action / F310 0xC216, WingMan 0xC211)
    if (s_ctx.vid == 0x046D && (s_ctx.pid == 0xC218 || s_ctx.pid == 0xC219 || s_ctx.pid == 0xC216 || s_ctx.pid == 0xC211) && len >= 6) {
        s_state.lx = rx[0];
        s_state.ly = rx[1];
        s_state.rx = rx[2];
        s_state.ry = rx[3];

        uint8_t hat = rx[4] & 0x0F;
        uint32_t b = 0;
        if (hat < 8) b |= dirs[hat];

        // Buttons 1-4 (Square, Cross, Circle, Triangle) from high nibble of byte 4
        if (rx[4] & 0x10) b |= s_ctx.button_map[0]; // Button 1 -> Square
        if (rx[4] & 0x20) b |= s_ctx.button_map[1]; // Button 2 -> Cross
        if (rx[4] & 0x40) b |= s_ctx.button_map[2]; // Button 3 -> Circle
        if (rx[4] & 0x80) b |= s_ctx.button_map[3]; // Button 4 -> Triangle

        // Buttons 5-12 (L1, R1, L2, R2, Select, Start, L3, R3) from byte 5
        for (int i = 0; i < 8; i++) {
            if (rx[5] & (1 << i)) {
                b |= s_ctx.button_map[4 + i];
            }
        }

        s_state.buttons = b;
        s_state.l2 = (b & DS4_BUTTON_L2) ? 255 : 0;
        s_state.r2 = (b & DS4_BUTTON_R2) ? 255 : 0;

        uint8_t deadzone = cfg ? cfg->stick_deadzone : 15;
        out->leftStick.x  = translator_apply_deadzone(s_state.lx, deadzone);
        out->leftStick.y  = translator_apply_deadzone(s_state.ly, deadzone);
        out->rightStick.x = translator_apply_deadzone(s_state.rx, deadzone);
        out->rightStick.y = translator_apply_deadzone(s_state.ry, deadzone);
        out->analogButtons.l2 = s_state.l2;
        out->analogButtons.r2 = s_state.r2;
        out->buttons = s_state.buttons;
        out->connected = 1;
        return 1;
    }

    // Dedicated deterministic parsing for ShanWan and Betop DirectInput gamepads
    // (VID 0x2563: BSP-G6, PS3/PC Gamepad, Redragon G808; VID 0x20BC: Betop 2585N2, BTP-2185)
    if ((s_ctx.vid == 0x2563 || s_ctx.vid == 0x20BC) && len >= 6) {
        // ShanWan and Betop hardware chipsets prepend Report ID (typically 0x01) on standard gamepad reports.
        // Never dynamically condition off on stick axes (e.g. rx[1]), as horizontal stick movements
        // take rx[1] towards 0 or 255 which would incorrectly toggle the report offset.
        int off = (len >= 7 && rx[0] <= 4) ? 1 : 0;
        s_state.lx = rx[off + 0];
        s_state.ly = rx[off + 1];
        s_state.rx = rx[off + 2];
        s_state.ry = rx[off + 3];

        uint8_t hat = rx[off + 4] & 0x0F;
        uint32_t b = 0;
        if (hat < 8) b |= dirs[hat];

        // Buttons 1-4 (Square, Cross, Circle, Triangle) from high nibble of byte 4
        if (rx[off + 4] & 0x10) b |= s_ctx.button_map[0]; // Button 1 -> Square
        if (rx[off + 4] & 0x20) b |= s_ctx.button_map[1]; // Button 2 -> Cross
        if (rx[off + 4] & 0x40) b |= s_ctx.button_map[2]; // Button 3 -> Circle
        if (rx[off + 4] & 0x80) b |= s_ctx.button_map[3]; // Button 4 -> Triangle

        // Buttons 5-12 (L1, R1, L2, R2, Select, Start, L3, R3) from byte 5
        if (len >= off + 6) {
            for (int i = 0; i < 8; i++) {
                if (rx[off + 5] & (1 << i)) {
                    b |= s_ctx.button_map[4 + i];
                }
            }
        }

        // Mode / Home button (bit 0 of byte 6)
        if (len >= off + 7 && (rx[off + 6] & 0x01)) {
            b |= DS4_BUTTON_PS;
        }

        s_state.buttons = b;
        s_state.l2 = (b & DS4_BUTTON_L2) ? 255 : 0;
        s_state.r2 = (b & DS4_BUTTON_R2) ? 255 : 0;

        uint8_t deadzone = cfg ? cfg->stick_deadzone : 15;
        out->leftStick.x  = translator_apply_deadzone(s_state.lx, deadzone);
        out->leftStick.y  = translator_apply_deadzone(s_state.ly, deadzone);
        out->rightStick.x = translator_apply_deadzone(s_state.rx, deadzone);
        out->rightStick.y = translator_apply_deadzone(s_state.ry, deadzone);
        out->analogButtons.l2 = s_state.l2;
        out->analogButtons.r2 = s_state.r2;
        out->buttons = s_state.buttons;
        out->connected = 1;
        return 1;
    }

    const uint8_t *d = rx;
    uint8_t rid = 0;

    if (s_ctx.has_ids) {
        if (len < 2) return 0;
        rid = rx[0];
        d = rx + 1;
        len--;
    }

    uint8_t *out_axes[ROLE_N] = { &s_state.lx, &s_state.ly, &s_state.rx, &s_state.ry, &s_state.l2, &s_state.r2 };
    uint32_t b = 0, mask = 0;
    int used = 0, trig[2] = { 0, 0 };

    // Update axes
    for (int i = 0; i < ROLE_N; i++) {
        if (s_ctx.role[i] == AX_NONE) continue;
        const hid_field *f = &s_ctx.axis[s_ctx.role[i]];
        if (!f->present || f->rid != rid) continue;
        uint8_t v = to_byte(f, get_bits(d, len, f));
        *out_axes[i] = s_ctx.invert[i] ? (uint8_t)(255 - v) : v;
        if (i == ROLE_L2) trig[0] = 1;
        if (i == ROLE_R2) trig[1] = 1;
        used = 1;
    }

    // Update digital buttons
    for (int i = 0; i < s_ctx.nbuttons; i++) {
        const hid_field *f = &s_ctx.button[i];
        if (!f->present || f->rid != rid) continue;
        mask |= s_ctx.button_map[i];
        if (get_bits(d, len, f)) b |= s_ctx.button_map[i];
        used = 1;
    }

    // Update HAT D-Pad
    if (s_ctx.axis[AX_HAT].present && s_ctx.axis[AX_HAT].rid == rid) {
        const hid_field *f = &s_ctx.axis[AX_HAT];
        int32_t h = get_bits(d, len, f) - f->min;
        mask |= DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_LEFT | DS4_BUTTON_DPAD_RIGHT;
        if (h >= 0 && h < 8) b |= dirs[h];
        used = 1;
    }

    // Home / Back
    if (s_ctx.home.present && s_ctx.home.rid == rid) {
        mask |= DS4_BUTTON_PS;
        if (get_bits(d, len, &s_ctx.home)) b |= DS4_BUTTON_PS;
        used = 1;
    }
    if (s_ctx.back.present && s_ctx.back.rid == rid) {
        uint32_t back_target = (g_config.back_button_action == CONFIG_BACK_TOUCHPAD) ? DS4_BUTTON_TOUCHPAD : DS4_BUTTON_SHARE;
        mask |= back_target;
        if (get_bits(d, len, &s_ctx.back)) b |= back_target;
        used = 1;
    }

    // If report was not parsed via descriptor fields (e.g. descriptor read failed)
    if (!used && !s_configured && len >= 5) {
        int off = 0;
        if (rx[0] <= 4 && len >= 6 && rx[1] >= 80 && rx[1] <= 180) {
            off = 1; // rx[0] is Report ID
        }
        s_state.lx = rx[off + 0];
        s_state.ly = rx[off + 1];
        s_state.rx = rx[off + 2];
        s_state.ry = rx[off + 3];
        uint8_t hat = rx[off + 4] & 0x0F;
        mask |= DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_LEFT | DS4_BUTTON_DPAD_RIGHT;
        if (hat < 8) b |= dirs[hat];
        if (len >= off + 6) {
            uint16_t btns = (uint16_t)rx[off + 5] | ((uint16_t)(len >= off + 7 ? rx[off + 6] : 0) << 8);
            uint8_t nibble_btns = (rx[off + 4] >> 4) & 0x0F;
            if (nibble_btns != 0 && (btns & 0x0F) == 0) {
                // Handle pads where buttons 1-4 are packed into the high nibble of byte 4
                btns = ((uint16_t)btns << 4) | nibble_btns;
            }
            int max_fallback_btns = (s_ctx.nbuttons > 0 && s_ctx.nbuttons <= 14) ? s_ctx.nbuttons : 12;
            for (int i = 0; i < max_fallback_btns; i++) {
                // In unconfigured raw fallback, ignore phantom PS / Touchpad bindings from telemetry bits
                if (!s_configured && (s_ctx.button_map[i] & (DS4_BUTTON_PS | DS4_BUTTON_TOUCHPAD))) {
                    continue;
                }
                mask |= s_ctx.button_map[i];
                if (btns & (1 << i)) b |= s_ctx.button_map[i];
            }
        }
        used = 1;
    }

    if (!used) return 0;

    if (trig[0]) { mask |= DS4_BUTTON_L2; if (s_state.l2 > 0x20) b |= DS4_BUTTON_L2; }
    if (trig[1]) { mask |= DS4_BUTTON_R2; if (s_state.r2 > 0x20) b |= DS4_BUTTON_R2; }

    s_state.buttons = (s_state.buttons & ~mask) | b;
    if (s_ctx.role[ROLE_L2] == AX_NONE && (mask & DS4_BUTTON_L2)) s_state.l2 = (b & DS4_BUTTON_L2) ? 255 : 0;
    if (s_ctx.role[ROLE_R2] == AX_NONE && (mask & DS4_BUTTON_R2)) s_state.r2 = (b & DS4_BUTTON_R2) ? 255 : 0;

    // Output to OrbisPadData with deadzone applied
    // Note: Standard HID already reports Y-axis with 0=UP and 255=DOWN matching PS4 DualShock 4
    uint8_t deadzone = cfg ? cfg->stick_deadzone : 15;
    out->leftStick.x  = translator_apply_deadzone(s_state.lx, deadzone);
    out->leftStick.y  = translator_apply_deadzone(s_state.ly, deadzone);
    out->rightStick.x = translator_apply_deadzone(s_state.rx, deadzone);
    out->rightStick.y = translator_apply_deadzone(s_state.ry, deadzone);
    out->analogButtons.l2 = s_state.l2;
    out->analogButtons.r2 = s_state.r2;
    out->buttons = s_state.buttons;
    out->connected = 1;

    return 1;
}

static void send_hid_output_packet(libusb_device_handle* h, uint8_t ep_out, uint8_t iface,
                                   uint8_t report_id, uint8_t* pkt, int len,
                                   int (*intr_transfer)(libusb_device_handle*, uint8_t, uint8_t*, int32_t, int32_t*, uint32_t),
                                   int (*ctrl_transfer)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, uint32_t)) {
    if (!h || !pkt || len <= 0) return;

    int transferred = 0;
    int sent = 0;
    if (ep_out != 0 && intr_transfer) {
        if (intr_transfer(h, ep_out, pkt, len, &transferred, 16) == 0) {
            sent = 1;
        }
    }
    // Never fall back to EP0 SET_REPORT on Logitech (0x046D), ShanWan (0x2563) or Betop (0x20BC),
    // as control transfers trigger the LG_NOGET hardware bug, stalling EP_IN and locking motors.
    bool is_noget_quirk = (s_ctx.vid == 0x046D || s_ctx.vid == 0x2563 || s_ctx.vid == 0x20BC);
    if (!sent && ctrl_transfer && !is_noget_quirk) {
        // HID Class-Specific Request: SET_REPORT (bRequest=0x09), Report Type: Output (0x02)
        uint16_t wValue = (uint16_t)(0x0200 | report_id);
        ctrl_transfer(h, 0x21, 0x09, wValue, (uint16_t)iface, pkt, (uint16_t)len, 10);
    }
}

void generic_hid_send_rumble(libusb_device_handle* h, uint16_t vid, uint16_t pid,
                             uint8_t ep_out, uint8_t iface,
                             uint8_t large_motor, uint8_t small_motor,
                             int (*intr_transfer)(libusb_device_handle*, uint8_t, uint8_t*, int32_t, int32_t*, uint32_t),
                             int (*ctrl_transfer)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, uint32_t)) {
    if (!h) return;

    // 1. Check if explicitly disabled via `rumble = none`
    if (s_ctx.custom_rumble_len == 0xFF) {
        return;
    }

    // 2. Logitech DirectInput (VID 0x046D — RumblePad 2 0xC218, Cordless 0xC219, F310/F710, Dual Action) [Linux hid-lg2ff.c / hid-lgff.c]
    // Must run BEFORE custom_rumble_len template handling so `rumble = logitech` in 046D_C218.ini
    // never triggers an EP0 SET_REPORT control transfer (LG_NOGET hardware quirk) or a 2ms timeout on 10ms bInterval.
    // Per Linux drivers/hid/hid-lg2ff.c:
    // - When playing (large_motor > 0 || small_motor > 0): 0x51, 0x00, weak, 0x00, strong, 0x00, 0x00
    // - When stopping (large_motor == 0 && small_motor == 0): 0xF3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    if (vid == 0x046D) {
        uint8_t pkt[7];
        if (large_motor == 0 && small_motor == 0) {
            uint8_t stop[7] = { 0xF3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            memcpy(pkt, stop, sizeof(pkt));
        } else {
            uint8_t play[7] = { 0x51, 0x00, small_motor, 0x00, large_motor, 0x00, 0x00 };
            memcpy(pkt, play, sizeof(pkt));
        }
        // LG_NOGET hardware quirk: sending EP0 control transfers crashes the controller.
        // Send only via interrupt transfer with a 16ms timeout (>= 10ms bInterval).
        if (ep_out != 0 && intr_transfer) {
            int transferred = 0;
            intr_transfer(h, ep_out, pkt, sizeof(pkt), &transferred, 16);
        }
        return;
    }

    // 3. User-defined custom rumble template in /data/onefamily/controllers/VVVV_PPPP.ini
    if (s_ctx.custom_rumble_len > 0) {
        uint8_t pkt[32];
        int len = 0;
        for (int i = 0; i < s_ctx.custom_rumble_len && len < (int)sizeof(pkt) - 2; i++) {
            switch (s_ctx.custom_rumble[i].type) {
                case RUMBLE_TOKEN_LARGE:
                    pkt[len++] = large_motor;
                    break;
                case RUMBLE_TOKEN_SMALL:
                    pkt[len++] = small_motor;
                    break;
                case RUMBLE_TOKEN_LARGE_HALF:
                    pkt[len++] = (uint8_t)(large_motor >> 1);
                    break;
                case RUMBLE_TOKEN_SMALL_HALF:
                    pkt[len++] = (uint8_t)(small_motor >> 1);
                    break;
                case RUMBLE_TOKEN_LARGE_16LE: {
                    uint16_t v = (uint16_t)large_motor * 257;
                    pkt[len++] = (uint8_t)(v & 0xFF);
                    pkt[len++] = (uint8_t)(v >> 8);
                    break;
                }
                case RUMBLE_TOKEN_SMALL_16LE: {
                    uint16_t v = (uint16_t)small_motor * 257;
                    pkt[len++] = (uint8_t)(v & 0xFF);
                    pkt[len++] = (uint8_t)(v >> 8);
                    break;
                }
                default:
                    pkt[len++] = s_ctx.custom_rumble[i].val;
                    break;
            }
        }
        if (len > 0) {
            send_hid_output_packet(h, ep_out, iface, pkt[0], pkt, len, intr_transfer, ctrl_transfer);
        }
        return;
    }

    // 2. Level 2: Known Linux Kernel hid-*ff.c Hardware Chipset Protocols
    // 2a. 8BitDo (VID 0x2DC8): Vendor Report 0x81 + D-Input Report 0x05
    if (vid == 0x2DC8) {
        int transferred = 0;
        // 81 11 04 08 <dur16 LE> <left> <right> (duration: 0x0400 = 1024 ms)
        uint8_t pkt81[8] = { 0x81, 0x11, 0x04, 0x08, 0x00, 0x04, large_motor, small_motor };
        if (ep_out != 0 && intr_transfer) {
            intr_transfer(h, ep_out, pkt81, sizeof(pkt81), &transferred, 2);
        }
        if (ctrl_transfer) {
            ctrl_transfer(h, 0x21, 0x09, 0x0281, (uint16_t)iface, pkt81, sizeof(pkt81), 2);
        }
        if (s_ctx.out_present && s_ctx.out_rid == 0x05) {
            uint8_t pkt05[5] = { 0x05, large_motor, small_motor, 0x00, 0x00 };
            send_hid_output_packet(h, ep_out, iface, 0x05, pkt05, sizeof(pkt05), intr_transfer, ctrl_transfer);
        }
        return;
    }

    // 2c. DragonRise / Microntek (VID 0x0079) [Linux hid-dr.c]
    if (vid == 0x0079) {
        uint8_t pkt1[7] = { 0xFA, 0xFE, large_motor, small_motor, 0x00, 0x00, 0x00 };
        uint8_t pkt2[7] = { 0xF3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        send_hid_output_packet(h, ep_out, iface, 0x00, pkt1, sizeof(pkt1), intr_transfer, ctrl_transfer);
        send_hid_output_packet(h, ep_out, iface, 0x00, pkt2, sizeof(pkt2), intr_transfer, ctrl_transfer);
        return;
    }

    // 2d. GreenAsia / Pantherlord (VID 0x0E8F) [Linux hid-gaff.c / hid-pl.c]
    if (vid == 0x0E8F) {
        uint8_t pkt1[4] = { 0x51, 0x00, (uint8_t)(small_motor >> 1), (uint8_t)(large_motor >> 1) };
        uint8_t pkt2[4] = { 0xFA, 0xFE, 0x00, 0x00 };
        send_hid_output_packet(h, ep_out, iface, 0x00, pkt1, sizeof(pkt1), intr_transfer, ctrl_transfer);
        send_hid_output_packet(h, ep_out, iface, 0x00, pkt2, sizeof(pkt2), intr_transfer, ctrl_transfer);
        return;
    }

    // 2e. Betop / ShanWan (VID 0x20BC, 0x2563) [Linux hid-betopff.c]
    if (vid == 0x20BC || vid == 0x2563) {
        uint8_t pkt[4] = { 0x00, 0x00, large_motor, small_motor };
        send_hid_output_packet(h, ep_out, iface, 0x00, pkt, sizeof(pkt), intr_transfer, ctrl_transfer);
        return;
    }

    // 2f. ZeroPlus / Thrustmaster / Mega World (VID 0x0C12, 0x044F, 0x07B5) [Linux hid-zpff.c / hid-tmff.c]
    if (vid == 0x0C12 || vid == 0x044F || vid == 0x07B5) {
        uint8_t pkt[4] = { 0x00, small_motor, large_motor, 0x00 };
        send_hid_output_packet(h, ep_out, iface, 0x00, pkt, sizeof(pkt), intr_transfer, ctrl_transfer);
        return;
    }

    // 2g. SmartJoy / WiseGroup (VID 0x0925, 0x6666) [Linux hid-sjoy.c]
    if (vid == 0x0925 || vid == 0x6666) {
        uint8_t pkt[3] = { 0x01, large_motor, small_motor };
        send_hid_output_packet(h, ep_out, iface, 0x01, pkt, sizeof(pkt), intr_transfer, ctrl_transfer);
        return;
    }

    // 2h. Holtek (VID 0x1241) [Linux hid-holtekff.c]
    if (vid == 0x1241) {
        uint8_t pkt[7] = { 0x00, 0x01, 0x00, 0x00, large_motor, small_motor, 0x00 };
        send_hid_output_packet(h, ep_out, iface, 0x00, pkt, sizeof(pkt), intr_transfer, ctrl_transfer);
        return;
    }

    // 3. Level 3: Auto-Discovered Output Report from HID Report Descriptor (tag == 9)
    if (s_ctx.out_present && s_ctx.out_bits >= 16) {
        uint8_t pkt[64];
        memset(pkt, 0, sizeof(pkt));

        int body_bytes = (s_ctx.out_bits + 7) / 8;
        if (body_bytes < 2) body_bytes = 2;
        if (body_bytes > 62) body_bytes = 62;

        int pos = 0;
        if (s_ctx.out_has_ids) {
            pkt[pos++] = s_ctx.out_rid;
        }

        int data_off = pos + (s_ctx.out_data_bit_off / 8);
        int total_len = pos + body_bytes;

        if (s_ctx.out_field_size == 16 && data_off + 4 <= total_len) {
            uint16_t strong = (uint16_t)large_motor * 257;
            uint16_t weak   = (uint16_t)small_motor * 257;
            pkt[data_off + 0] = (uint8_t)(strong & 0xFF);
            pkt[data_off + 1] = (uint8_t)(strong >> 8);
            pkt[data_off + 2] = (uint8_t)(weak & 0xFF);
            pkt[data_off + 3] = (uint8_t)(weak >> 8);
        } else if (data_off + 2 <= total_len) {
            pkt[data_off + 0] = large_motor;
            pkt[data_off + 1] = small_motor;
        }

        send_hid_output_packet(h, ep_out, iface, s_ctx.out_rid, pkt, total_len, intr_transfer, ctrl_transfer);
        return;
    }

    // 4. Level 4: Universal Fallback (Standard D-Input Report 0x05 + Generic 8-byte Output Report)
    uint8_t pkt05[5] = { 0x05, large_motor, small_motor, 0x00, 0x00 };
    uint8_t pkt00[8] = { 0x00, large_motor, small_motor, 0x00, 0x00, 0x00, 0x00, 0x00 };
    send_hid_output_packet(h, ep_out, iface, 0x05, pkt05, sizeof(pkt05), intr_transfer, ctrl_transfer);
    send_hid_output_packet(h, ep_out, iface, 0x00, pkt00, sizeof(pkt00), intr_transfer, ctrl_transfer);
}
