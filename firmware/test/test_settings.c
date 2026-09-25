/* profiles/settings.c (registry, text, sparse blob) and settings_persist.c (debounced save) over a
 * simulated flash. */
#include "settings.h"
#include "settings_persist.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- fake "modules": the real source of truth for each setting ---- */
static uint32_t m_mode, m_enabled, m_aftertouch;
static float m_bend, m_gain;
static int32_t m_ref_note;
static uint32_t m_din, m_look_natural, m_volatile;
static int set_calls;

static tiles_setting_value_t U(uint32_t v) { tiles_setting_value_t x; x.u = v; return x; }
static tiles_setting_value_t I(int32_t v) { tiles_setting_value_t x; x.i = v; return x; }
static tiles_setting_value_t F(float v) { tiles_setting_value_t x; x.f = v; return x; }

static tiles_setting_value_t g_mode(void) { return U(m_mode); }            static void s_mode(tiles_setting_value_t v) { m_mode = v.u; set_calls++; }
static tiles_setting_value_t g_enabled(void) { return U(m_enabled); }      static void s_enabled(tiles_setting_value_t v) { m_enabled = v.u; set_calls++; }
static tiles_setting_value_t g_at(void) { return U(m_aftertouch); }        static void s_at(tiles_setting_value_t v) { m_aftertouch = v.u; set_calls++; }
static tiles_setting_value_t g_bend(void) { return F(m_bend); }            static void s_bend(tiles_setting_value_t v) { m_bend = v.f; set_calls++; }
static tiles_setting_value_t g_gain(void) { return F(m_gain); }            static void s_gain(tiles_setting_value_t v) { m_gain = v.f; set_calls++; }
static tiles_setting_value_t g_ref(void) { return I(m_ref_note); }         static void s_ref(tiles_setting_value_t v) { m_ref_note = v.i; set_calls++; }
static tiles_setting_value_t g_din(void) { return U(m_din); }              static void s_din(tiles_setting_value_t v) { m_din = v.u; set_calls++; }
static tiles_setting_value_t g_vol(void) { return U(m_volatile); }        static void s_vol(tiles_setting_value_t v) { m_volatile = v.u; set_calls++; }
static tiles_setting_value_t g_nat(void) { return U(m_look_natural); }     static void s_nat(tiles_setting_value_t v) { m_look_natural = v.u; set_calls++; }

static const char *const MODE_NAMES[] = {"sustain", "expression"};
static const char *const DIN_NAMES[] = {"a", "b"};
static const tiles_setting_def_t TABLE[] = {
    TILES_SETTING(256,  "pedal.mode",            TILES_SETTING_ENUM,  {.u = 0},   {.u = 1},     MODE_NAMES, g_mode, s_mode),
    TILES_SETTING(512,  "expression.enabled",    TILES_SETTING_BOOL,  {.u = 0},   {.u = 1},     NULL, g_enabled, s_enabled),
    TILES_SETTING(514,  "expression.aftertouch", TILES_SETTING_UINT,  {.u = 1},   {.u = 65535}, NULL, g_at, s_at),
    TILES_SETTING(513,  "expression.bend",       TILES_SETTING_FLOAT, {.f = 0.001f}, {.f = 1.0f}, NULL, g_bend, s_bend),
    TILES_SETTING(775,  "cv.gain_trim",          TILES_SETTING_FLOAT, {.f = 0.5f}, {.f = 2.0f}, NULL, g_gain, s_gain),
    TILES_SETTING(770,  "cv.reference_note",     TILES_SETTING_INT,   {.i = -12}, {.i = 127},   NULL, g_ref, s_ref),
    TILES_SETTING(1280, "midi.din_trs_type",     TILES_SETTING_ENUM,  {.u = 0},   {.u = 1},     DIN_NAMES, g_din, s_din),
    TILES_SETTING(1025, "look.natural_pad_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100},   NULL, g_nat, s_nat),
    TILES_SETTING_V(768, "cv.enabled",           TILES_SETTING_BOOL,  {.u = 0},   {.u = 1},     NULL, g_vol, s_vol),
};
#define N (sizeof(TABLE) / sizeof(TABLE[0]))

static void reset_modules(void) {
    m_mode = 0; m_enabled = 1; m_aftertouch = 1450; m_bend = 0.065f; m_gain = 1.0f; m_ref_note = 0; m_din = 0; m_look_natural = 21; m_volatile = 0;
    set_calls = 0;
}
static void boot_registry(void) { reset_modules(); tiles_settings_init(TABLE, N); tiles_settings_capture_defaults(); }

/* ---- simulated NOR flash with failure injection ---- */
static uint8_t g_mem[TILES_KV_NUM_SLOTS][TILES_KV_SECTOR_SIZE];
static bool g_fail_erase;
static int g_erases;
static bool sim_read(uint8_t s, uint32_t o, uint8_t *b, uint32_t l) { memcpy(b, &g_mem[s][o], l); return true; }
static bool sim_erase(uint8_t s) { if (g_fail_erase) return false; g_erases++; memset(g_mem[s], 0xFF, TILES_KV_SECTOR_SIZE); return true; }
static bool sim_program(uint8_t s, uint32_t o, const uint8_t *b, uint32_t l) {
    for (uint32_t i = 0; i < l; i++) { assert((g_mem[s][o + i] & b[i]) == b[i]); g_mem[s][o + i] &= b[i]; } return true; }
static const tiles_kv_ops_t OPS = {sim_read, sim_erase, sim_program};

static bool g_idle = true;
static bool idle_fn(void) { return g_idle; }

static void text_of(const char *key, char *out) { assert(tiles_settings_get_text(key, out, 64)); }

int main(void) {
    char buf[200];

    /* ---------- registry & text ---------- */
    boot_registry();
    assert(tiles_settings_count() == N);
    assert(tiles_settings_find_key("pedal.mode") == &TABLE[0] && tiles_settings_find_id(770) == &TABLE[5]);
    assert(!tiles_settings_find_key("nope") && !tiles_settings_find_id(9));
    text_of("pedal.mode", buf); assert(!strcmp(buf, "sustain"));
    text_of("expression.aftertouch", buf); assert(!strcmp(buf, "1450"));
    text_of("expression.bend", buf); assert(!strcmp(buf, "0.065000"));
    text_of("cv.reference_note", buf); assert(!strcmp(buf, "0"));

    assert(tiles_settings_set_text("pedal.mode", "expression") == TILES_SETTINGS_OK && m_mode == 1);
    assert(tiles_settings_set_text("pedal.mode", "bogus") == TILES_SETTINGS_OUT_OF_RANGE && m_mode == 1);   /* untouched */
    assert(tiles_settings_set_text("pedal.mode", "") == TILES_SETTINGS_BAD_VALUE);
    assert(tiles_settings_set_text("nope", "1") == TILES_SETTINGS_UNKNOWN_KEY);
    assert(tiles_settings_set_text("expression.enabled", "1") == TILES_SETTINGS_OK);
    assert(tiles_settings_set_text("expression.enabled", "true") == TILES_SETTINGS_BAD_VALUE);
    assert(tiles_settings_set_text("expression.enabled", "2") == TILES_SETTINGS_BAD_VALUE);
    assert(tiles_settings_set_text("expression.aftertouch", "0") == TILES_SETTINGS_OUT_OF_RANGE);          /* min is 1 */
    assert(tiles_settings_set_text("expression.aftertouch", "65535") == TILES_SETTINGS_OK && m_aftertouch == 65535);
    assert(tiles_settings_set_text("expression.aftertouch", "65536") == TILES_SETTINGS_OUT_OF_RANGE);
    assert(tiles_settings_set_text("expression.aftertouch", "-5") == TILES_SETTINGS_BAD_VALUE);
    assert(tiles_settings_set_text("expression.aftertouch", "12x") == TILES_SETTINGS_BAD_VALUE);
    assert(tiles_settings_set_text("expression.bend", "0.5") == TILES_SETTINGS_OK && fabsf(m_bend - 0.5f) < 1e-6f);
    assert(tiles_settings_set_text("expression.bend", "nan") == TILES_SETTINGS_BAD_VALUE);
    assert(tiles_settings_set_text("expression.bend", "1.0001") == TILES_SETTINGS_OUT_OF_RANGE);
    assert(tiles_settings_set_text("expression.bend", "1.5x") == TILES_SETTINGS_BAD_VALUE);
    assert(tiles_settings_set_text("cv.reference_note", "-12") == TILES_SETTINGS_OK && m_ref_note == -12);   /* signed, inclusive min */
    assert(tiles_settings_set_text("cv.reference_note", "-13") == TILES_SETTINGS_OUT_OF_RANGE);
    assert(tiles_settings_set_text("midi.din_trs_type", "b") == TILES_SETTINGS_OK && m_din == 1);

    /* describe: what the app builds its UI from */
    tiles_settings_describe(&TABLE[0], buf, sizeof(buf)); assert(!strcmp(buf, "id=256 key=pedal.mode type=enum values=sustain|expression default=sustain"));
    tiles_settings_describe(&TABLE[1], buf, sizeof(buf)); assert(!strcmp(buf, "id=512 key=expression.enabled type=bool default=1"));
    tiles_settings_describe(&TABLE[2], buf, sizeof(buf)); assert(!strcmp(buf, "id=514 key=expression.aftertouch type=uint min=1 max=65535 default=1450"));
    tiles_settings_describe(&TABLE[3], buf, sizeof(buf)); assert(!strcmp(buf, "id=513 key=expression.bend type=float min=0.001 max=1 default=0.065000"));
    tiles_settings_describe(&TABLE[5], buf, sizeof(buf)); assert(!strcmp(buf, "id=770 key=cv.reference_note type=int min=-12 max=127 default=0"));

    /* reset: one key, then everything */
    assert(tiles_settings_reset("pedal.mode") && m_mode == 0 && m_din == 1);
    assert(!tiles_settings_reset("nope"));
    assert(tiles_settings_reset(NULL) && m_din == 0 && m_aftertouch == 1450 && m_ref_note == 0 && fabsf(m_bend - 0.065f) < 1e-6f);

    /* ---------- sparse blob ---------- */
    boot_registry();
    uint8_t blob[512]; bool ov;
    assert(tiles_settings_serialize(blob, sizeof(blob), &ov) == 0 && !ov);            /* all defaults -> empty */
    m_mode = 1; m_look_natural = 30;
    size_t n = tiles_settings_serialize(blob, sizeof(blob), &ov);
    assert(n == 14 && !ov);                                                            /* two entries x 7 bytes */
    uint8_t blob2[512]; assert(tiles_settings_serialize(blob2, sizeof(blob2), &ov) == 14 && !memcmp(blob, blob2, 14));  /* deterministic */
    m_look_natural = 21; assert(tiles_settings_serialize(blob2, sizeof(blob2), &ov) == 7);           /* back to default -> dropped */
    m_look_natural = 30;
    assert(tiles_settings_serialize(blob, 10, &ov) == 0 && ov);                        /* 14 bytes needed, 10 given: reported, not truncated */

    /* apply into a fresh boot */
    m_mode = 1; m_look_natural = 30; m_bend = 0.25f; m_ref_note = 60;
    n = tiles_settings_serialize(blob, sizeof(blob), &ov); assert(n == 28);
    boot_registry();
    assert(tiles_settings_apply_blob(blob, n) == 4 && m_mode == 1 && m_look_natural == 30 && m_ref_note == 60 && fabsf(m_bend - 0.25f) < 1e-6f);
    /* skipped safely: unknown id, changed type, out-of-range value, truncated tail */
    boot_registry();
    uint8_t bad[7 * 5 + 3]; memset(bad, 0, sizeof(bad));
    bad[0] = 0xEF; bad[1] = 0xBE; bad[2] = 0; bad[3] = 5;                              /* id 0xBEEF: unknown */
    bad[7] = 0x00; bad[8] = 0x01; bad[9] = TILES_SETTING_UINT; bad[10] = 1;            /* id 256 stored as the wrong type */
    bad[14] = 0x01; bad[15] = 0x04; bad[16] = TILES_SETTING_UINT; bad[17] = 200;       /* id 1025 = 200: out of range */
    bad[21] = 0x01; bad[22] = 0x04; bad[23] = TILES_SETTING_UINT; bad[24] = 44;        /* id 1025 = 44: fine */
    assert(tiles_settings_apply_blob(bad, sizeof(bad)) == 1 && m_look_natural == 44 && m_mode == 0);
    uint32_t nan_bits = 0x7fc00000u; boot_registry();
    uint8_t nanb[7] = {0x01, 0x02, TILES_SETTING_FLOAT, 0, 0, 0xc0, 0x7f}; (void)nan_bits;
    assert(tiles_settings_apply_blob(nanb, sizeof(nanb)) == 0 && fabsf(m_bend - 0.065f) < 1e-6f);   /* a stored NaN is refused */

    /* volatile: settable, resettable, reported by SCHEMA -- but never serialized or restored */
    boot_registry();
    assert(tiles_settings_set_text("cv.enabled", "1") == TILES_SETTINGS_OK && m_volatile == 1);
    assert(tiles_settings_serialize(blob, sizeof(blob), &ov) == 0);
    tiles_settings_describe(tiles_settings_find_key("cv.enabled"), buf, sizeof(buf));
    assert(!strcmp(buf, "id=768 key=cv.enabled type=bool default=0 persist=0"));
    uint8_t vol_blob[7] = {0x00, 0x03, TILES_SETTING_BOOL, 1, 0, 0, 0};
    m_volatile = 0; assert(tiles_settings_apply_blob(vol_blob, sizeof(vol_blob)) == 0 && m_volatile == 0);
    m_volatile = 1; assert(tiles_settings_reset(NULL) && m_volatile == 0);

    /* ---------- persistence ---------- */
    memset(g_mem, 0xFF, sizeof(g_mem)); g_erases = 0; g_fail_erase = false; g_idle = true;
    boot_registry(); tiles_settings_persist_init(&OPS, idle_fn);
    assert(!tiles_settings_persist_get_info().loaded);
    uint32_t t = 1000;
    for (int i = 0; i < 20; i++) { t += 500; tiles_settings_persist_service(t); }
    assert(g_erases == 0);                                                              /* untouched: never writes */

    tiles_settings_set_text("look.natural_pad_percent", "30");
    tiles_settings_persist_service(t += 500); assert(tiles_settings_persist_get_info().pending && g_erases == 0);
    tiles_settings_persist_service(t += 500); tiles_settings_persist_service(t += 500); assert(g_erases == 0);       /* debouncing */
    /* keeps changing -> the quiet period restarts */
    tiles_settings_set_text("look.natural_pad_percent", "31"); tiles_settings_persist_service(t += 500);
    tiles_settings_persist_service(t += 500); tiles_settings_persist_service(t += 500); tiles_settings_persist_service(t += 500); assert(g_erases == 0);
    tiles_settings_persist_service(t += 500); assert(g_erases == 1 && !tiles_settings_persist_get_info().pending);    /* one write, after the quiet period */
    for (int i = 0; i < 20; i++) tiles_settings_persist_service(t += 500);
    assert(g_erases == 1);                                                                                              /* no rewrite of an unchanged snapshot */
    assert(tiles_settings_persist_get_info().saved_bytes == 7);

    /* reboot: the saved value comes back */
    boot_registry(); tiles_settings_persist_init(&OPS, idle_fn);
    assert(tiles_settings_persist_get_info().loaded && tiles_settings_persist_get_info().applied == 1 && m_look_natural == 31);
    for (int i = 0; i < 10; i++) tiles_settings_persist_service(t += 500);
    assert(g_erases == 1);                                                              /* restoring what was saved isn't a change */

    /* not while playing */
    g_idle = false; tiles_settings_set_text("pedal.mode", "expression");
    for (int i = 0; i < 20; i++) tiles_settings_persist_service(t += 500);
    assert(g_erases == 1 && tiles_settings_persist_get_info().pending);                 /* held back, still pending */
    g_idle = true; tiles_settings_persist_service(t += 500); assert(g_erases == 2);

    /* a failing flash: retried later, not in a tight loop */
    tiles_settings_set_text("midi.din_trs_type", "b"); g_fail_erase = true;
    for (int i = 0; i < 6; i++) tiles_settings_persist_service(t += 500);
    uint32_t fails = tiles_settings_persist_get_info().save_failures; assert(fails == 1);
    for (int i = 0; i < 40; i++) tiles_settings_persist_service(t += 500);              /* 20 s: still inside the retry backoff */
    assert(tiles_settings_persist_get_info().save_failures == 1);
    g_fail_erase = false;
    for (int i = 0; i < 30; i++) tiles_settings_persist_service(t += 500);
    assert(tiles_settings_persist_get_info().save_failures == 1 && !tiles_settings_persist_get_info().pending);
    boot_registry(); tiles_settings_persist_init(&OPS, idle_fn); assert(m_din == 1 && m_mode == 1 && m_look_natural == 31);

    /* SAVE now, and "nothing to save" is a success that doesn't touch flash */
    int e = g_erases;
    assert(tiles_settings_persist_save_now() == TILES_KV_OK && g_erases == e);
    tiles_settings_set_text("look.natural_pad_percent", "50");
    assert(tiles_settings_persist_save_now() == TILES_KV_OK && g_erases == e + 1);
    boot_registry(); tiles_settings_persist_init(&OPS, idle_fn); assert(m_look_natural == 50);

    /* RESET ALL persists an empty snapshot (defaults win after a reboot) */
    tiles_settings_reset(NULL); assert(tiles_settings_persist_save_now() == TILES_KV_OK);
    boot_registry(); tiles_settings_persist_init(&OPS, idle_fn);
    assert(m_look_natural == 21 && m_din == 0 && m_mode == 0 && tiles_settings_persist_get_info().saved_bytes == 0);

    /* a newer firmware changes a default: a setting the user never touched follows it; one they set sticks */
    tiles_settings_set_text("expression.aftertouch", "2000"); tiles_settings_persist_save_now();
    reset_modules(); m_look_natural = 25; m_aftertouch = 1450;                            /* "new build": natural default 21 -> 25 */
    tiles_settings_init(TABLE, N); tiles_settings_capture_defaults(); tiles_settings_persist_init(&OPS, idle_fn);
    assert(m_look_natural == 25 && m_aftertouch == 2000);

    /* a snapshot with an unknown id is cleaned up on the next save; a blob from an unknown version is ignored */
    memset(g_mem, 0xFF, sizeof(g_mem)); boot_registry();
    tiles_kv_init(&OPS);
    uint8_t junk[14] = {0xEF, 0xBE, 0, 9, 0, 0, 0,   0x01, 0x04, TILES_SETTING_UINT, 40, 0, 0, 0};
    assert(tiles_kv_write(junk, sizeof(junk), TILES_SETTINGS_BLOB_VERSION) == TILES_KV_OK);
    boot_registry(); tiles_settings_persist_init(&OPS, idle_fn); assert(m_look_natural == 40 && tiles_settings_persist_get_info().saved_bytes == 14);
    e = g_erases; for (int i = 0; i < 20; i++) tiles_settings_persist_service(t += 500);
    assert(g_erases == e + 1 && tiles_settings_persist_get_info().saved_bytes == 7);       /* unknown entry dropped, known kept */
    memset(g_mem, 0xFF, sizeof(g_mem)); tiles_kv_init(&OPS);
    assert(tiles_kv_write(junk, sizeof(junk), 99) == TILES_KV_OK);
    boot_registry(); tiles_settings_persist_init(&OPS, idle_fn); assert(m_look_natural == 21 && !tiles_settings_persist_get_info().loaded);

    printf("settings: all tests pass\n");
    return 0;
}
