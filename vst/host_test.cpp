/* Offline x86 test of ccmap_vst.cpp (test.sh builds it with -DNO_ALSA and ASan/UBSan): the plugin
 * is an insert effect that passes audio through; LEARN (Launch Control knob and pad, UC-33, the
 * learning pad's release ignored), CC / NOTE / PROGRAM / PAGE targets, MIN > MAX inverts, MOMENT and
 * TOGGLE, one control on several slots, pages (arrows left/right, page keys up/down select the slot,
 * a held note is released when its page goes away), UNMAPPED thru, FOLLOW, the Launch Control
 * LEDs (pads, arrows, template change by SysEx), parameter echo from the host, CLEAR, factory
 * presets (Euclidier FX map), SAVE/LOAD in the shared bank file, the project chunk, and a random
 * parameter/MIDI stress run. The controllers come in through test hooks instead of ALSA.
 * Prints PASSED/FAILED. */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "forcehub.h"

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
enum { DEV_LC = 1, DEV_UC = 2, DEV_USB = 3, DEV_EC4 = 4, DEV_E16 = 5, FB = 16 };

static int fails = 0, automates = 0, last_auto_idx = -1;
static float last_auto_val = -1;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static AEffect *g_auto_target = nullptr;
static bool g_echo = false;   /* behave like MPC: hand an automate straight back as setParameter */
static intptr_t master(AEffect *e, int32_t op, int32_t idx, intptr_t, void *, float opt) {
    if (op == 0) {
        automates++; last_auto_idx = idx; last_auto_val = opt;
        if (g_echo && e && e == g_auto_target) e->setParameter(e, idx, opt);
    }
    return 0;
}

typedef void (*in_fn)(AEffect *, int, int, int, int);
typedef void (*sx_fn)(AEffect *, const uint8_t *, int);
typedef int (*take_fn)(AEffect *, uint8_t *, int);
typedef void (*leds_fn)(AEffect *, uint8_t *);
typedef int (*flash_fn)(AEffect *, uint8_t *);
static in_fn t_in;
static sx_fn t_sx;
static take_fn t_take;
static leds_fn t_leds;
static flash_fn t_flash;

struct Out { int port, st, d1, d2; };
static std::vector<Out> take(AEffect *e) {
    uint8_t b[4 * 512];
    int n = t_take(e, b, 512);
    std::vector<Out> v;
    for (int i = 0; i < n; i++) v.push_back({b[4 * i], b[4 * i + 1], b[4 * i + 2], b[4 * i + 3]});
    return v;
}
static std::string dump(const std::vector<Out> &v) {
    std::string s;
    char b[32];
    for (auto &o : v) { std::snprintf(b, sizeof b, "[%c %02X %d %d]", 'A' + o.port, o.st, o.d1, o.d2); s += b; }
    return s.empty() ? "(nothing)" : s;
}
static bool is(const std::vector<Out> &v, int port, int st, int d1, int d2) {
    return v.size() == 1 && v[0].port == port && v[0].st == st && v[0].d1 == d1 && v[0].d2 == d2;
}

static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
/* set a parameter in the units of module.json (switches by index) */
static void set(AEffect *e, const char *name, double v) {
    double lo = 0, hi = 1;
    const std::string s = name;
    if (s == "Slot") { lo = 1; hi = 128; }
    else if (s == "Page") hi = 4;
    else if (s == "Encoder" || s == "Feedback" || s == "LEDs") hi = 2;
    else if (s == "Web View" || s == "Files") hi = 1;
    else if (s == "Out" || s == "Active Page") hi = 3;
    else if (s == "Channel") { lo = 1; hi = 16; }
    else if (s == "Type") hi = 4;
    else if (s == "Number" || s == "Min" || s == "Max") hi = 127;
    else if (s == "Preset") { lo = 1; hi = 32; }
    e->setParameter(e, param(e, name), (float)((v - lo) / (hi - lo)));
}
static double get(AEffect *e, const char *name, double lo, double hi) { return lo + (hi - lo) * e->getParameter(e, param(e, name)); }
static void press(AEffect *e, const char *name) { int i = param(e, name); e->setParameter(e, i, 1.0f); e->setParameter(e, i, 0.0f); }
static void block(AEffect *e) {
    static float a[2][128], b[2][128];
    float *in[2] = {a[0], a[1]}, *out[2] = {b[0], b[1]};
    e->processReplacing(e, in, out, 128);
}

/* the phone view: one GET against 127.0.0.1:FORCEHUB_PORT, body only ("" on failure) */
static std::string http_full(const char *path_in, int port) {
    std::string pe;   /* a path with spaces: percent-encode them like a browser */
    for (const char *q = path_in; *q; q++) { if (*q == ' ') pe += "%20"; else pe += *q; }
    const char *path = pe.c_str();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string r;
    for (int t = 0; t < 20 && connect(fd, (sockaddr *)&a, sizeof a) < 0; t++) { usleep(50000); close(fd); fd = socket(AF_INET, SOCK_STREAM, 0); }
    std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
    if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0) { close(fd); return r; }
    char b[4096];
    ssize_t n;
    while ((n = recv(fd, b, sizeof b, 0)) > 0) r.append(b, (size_t)n);
    close(fd);
    return r;
}
static int hub_port() { return std::atoi(std::getenv("FORCEHUB_PORT")); }
static std::string http_get(const char *path, int port = 0) {
    const std::string r = http_full(path, port ? port : hub_port());
    size_t h = r.find("\r\n\r\n");
    return h == std::string::npos ? "" : r.substr(h + 4);
}
static AEffect *open_plugin(void *lib) {
    typedef AEffect *(*entry_t)(audioMasterCallback);
    entry_t entry = (entry_t)dlsym(lib, "VSTPluginMain");
    AEffect *e = entry(master);
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, 44100.0f);
    return e;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: host_test plugin.so\n"); return 2; }
    char bank[] = "/tmp/ccmap_test_bank_XXXXXX";
    int fd = mkstemp(bank);
    if (fd >= 0) close(fd);
    unlink(bank);
    setenv("CCMAP_PRESETS", bank, 1);
    char kitf[] = "/tmp/ccmap_test_kit_XXXXXX";
    fd = mkstemp(kitf);
    if (fd >= 0) close(fd);
    unlink(kitf);
    setenv("CCMAP_KIT", kitf, 1);
    if (!std::getenv("FORCEHUB_PORT")) setenv("FORCEHUB_PORT", "18080", 1);
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::printf("FAIL: dlopen %s\n", dlerror()); return 1; }
    t_in = (in_fn)dlsym(lib, "ccmap_test_input");
    t_sx = (sx_fn)dlsym(lib, "ccmap_test_sysex");
    t_take = (take_fn)dlsym(lib, "ccmap_test_take");
    t_leds = (leds_fn)dlsym(lib, "ccmap_test_leds");
    t_flash = (flash_fn)dlsym(lib, "ccmap_test_flash");
    if (!t_in || !t_sx || !t_take || !t_leds || !t_flash) { std::printf("FAIL: test hooks missing (build with -DNO_ALSA)\n"); return 1; }

    AEffect *e = open_plugin(lib);
    CHECK(e->magic == 0x56737450, "magic");
    CHECK(e->numInputs == 2 && e->numOutputs == 2, "insert effect: 2 in / 2 out (%d/%d)", e->numInputs, e->numOutputs);
    CHECK(e->dispatcher(e, 35, 0, 0, nullptr, 0) == 1, "category effect");
    CHECK(e->numPrograms == 32, "32 presets");
    {   /* audio passes through untouched */
        float a[2][64], b[2][64];
        for (int i = 0; i < 64; i++) { a[0][i] = std::sin(i * 0.1f); a[1][i] = -a[0][i]; }
        float *in[2] = {a[0], a[1]}, *out[2] = {b[0], b[1]};
        e->processReplacing(e, in, out, 64);
        bool same = true;
        for (int i = 0; i < 64; i++) same &= b[0][i] == a[0][i] && b[1][i] == a[1][i];
        CHECK(same, "audio pass-through");
        e->processReplacing(e, in, in, 64);   /* in == out */
    }

    CHECK(display(e, "Takeover") == "PICKUP" && display(e, "Feedback") == "EC4 + E16", "0.2.0 defaults: pickup, feedback to EC4");
    set(e, "Takeover", 0);   /* the 0.1.0 tests below expect jumps; PICKUP is tested further down */
    CHECK(display(e, "LEDs") == "ON + TEMPLATE", "0.3.0 default: template colour");
    CHECK(display(e, "LC Template") == "- (NO LC YET)", "no LC yet: '%s'", display(e, "LC Template").c_str());
    set(e, "LEDs", 1);       /* the 0.1.0 LED tests below expect plain slot colours */

    /* ---- learn a knob on slot 1 ---- */
    CHECK(display(e, "Slot") == "1 (empty)", "empty slot shows (empty): '%s'", display(e, "Slot").c_str());
    CHECK(display(e, "Source") == "- (LEARN)", "empty source: '%s'", display(e, "Source").c_str());
    press(e, "Learn");
    CHECK(display(e, "Source") == "MOVE A CONTROL ...", "learn armed: '%s'", display(e, "Source").c_str());
    t_in(e, DEV_LC, 0xB0, 25, 10);   /* LC user template 1, knob 5 */
    CHECK(take(e).empty(), "the learning move sends nothing");
    CHECK(display(e, "Source") == "LC U1 KNOB 5", "learned source: '%s'", display(e, "Source").c_str());
    CHECK(display(e, "Slot") == "1 LC U1 KNOB 5", "slot name: '%s'", display(e, "Slot").c_str());
    CHECK(display(e, "Source") != "MOVE A CONTROL ...", "learn switches itself off");
    press(e, "Learn"); press(e, "Learn");
    CHECK(display(e, "Source") == "LC U1 KNOB 5", "LEARN twice = cancel");
    CHECK(display(e, "Type") == "CC" && display(e, "Number") == "CC 25" && display(e, "Out") == "A", "fresh slot passes the CC on: %s %s %s",
          display(e, "Type").c_str(), display(e, "Number").c_str(), display(e, "Out").c_str());
    automates = 0;
    block(e);
    CHECK(automates > 0, "learn reported to the host from processReplacing");
    t_in(e, DEV_LC, 0xB0, 25, 64);
    auto o = take(e);
    CHECK(is(o, 0, 0xB0, 25, 64), "knob -> Out A CC 25: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xB0, 25, 64);
    CHECK(take(e).empty(), "same value is not sent twice");
    CHECK(display(e, "Last") == "LC U1 KNOB 5 = 64", "monitor: '%s'", display(e, "Last").c_str());

    /* retarget: Out C, channel 5, CC 74, range 127..0 (inverted) */
    set(e, "Out", 2); set(e, "Channel", 5); set(e, "Number", 74); set(e, "Min", 127); set(e, "Max", 0);
    t_in(e, DEV_LC, 0xB0, 25, 0);
    o = take(e);
    CHECK(is(o, 2, 0xB4, 74, 127), "inverted on Out C ch 5 CC 74: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xB0, 25, 127);
    o = take(e);
    CHECK(is(o, 2, 0xB4, 74, 0), "inverted top: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xB1, 25, 127);   /* same knob, other template: not this slot */
    CHECK(take(e).empty(), "other template does not match");

    /* ---- a pad on slot 2: toggle ---- */
    set(e, "Slot", 2);
    CHECK(display(e, "Slot") == "2 (empty)", "slot 2 empty");
    press(e, "Learn");
    t_in(e, DEV_LC, 0x90, 9, 127);    /* pad 1 */
    t_in(e, DEV_LC, 0x80, 9, 0);      /* its release must not matter */
    CHECK(take(e).empty(), "learning a pad sends nothing");
    CHECK(display(e, "Source") == "LC U1 PAD 1", "pad source: '%s'", display(e, "Source").c_str());
    CHECK(display(e, "Type") == "NOTE", "fresh pad -> NOTE");
    set(e, "Type", 1); set(e, "Number", 50); set(e, "Button", 1);
    uint8_t led[12];
    t_leds(e, led);
    CHECK(led[0] == 0x0D, "toggle off: dim red (%02X)", led[0]);
    t_in(e, DEV_LC, 0x90, 9, 127);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 50, 127), "toggle on: %s", dump(o).c_str());
    t_leds(e, led);
    CHECK(led[0] == 0x3C, "toggle on: green (%02X)", led[0]);
    t_in(e, DEV_LC, 0x80, 9, 0);
    CHECK(take(e).empty(), "toggle: release sends nothing");
    t_in(e, DEV_LC, 0x90, 9, 100);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 50, 0), "toggle off: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 9, 0);
    take(e);

    /* momentary note */
    set(e, "Button", 0); set(e, "Type", 2); set(e, "Number", 36); set(e, "Channel", 10);
    t_in(e, DEV_LC, 0x90, 9, 127);
    o = take(e);
    CHECK(is(o, 0, 0x99, 36, 127), "pad -> note on: %s", dump(o).c_str());
    CHECK(display(e, "Number") == "36  C1", "note name: '%s'", display(e, "Number").c_str());
    t_leds(e, led);
    CHECK(led[0] == 0x3F, "held pad amber (%02X)", led[0]);
    set(e, "Number", 38);   /* editing while held releases the old note */
    o = take(e);
    CHECK(is(o, 0, 0x89, 36, 0), "edit releases held note: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 9, 0);
    CHECK(take(e).empty(), "nothing left to release");

    /* ---- follow: touching knob 5 selects slot 1 ---- */
    t_in(e, DEV_LC, 0xB0, 25, 30);
    take(e);
    CHECK(display(e, "Slot").rfind("1 ", 0) == 0, "follow selects slot 1: '%s'", display(e, "Slot").c_str());
    set(e, "Follow", 0);
    set(e, "Slot", 2);
    t_in(e, DEV_LC, 0xB0, 25, 31);
    take(e);
    CHECK(display(e, "Slot").rfind("2 ", 0) == 0, "follow off keeps slot 2");
    set(e, "Follow", 1);

    /* ---- one knob, two slots ---- */
    set(e, "Slot", 3);
    press(e, "Learn");
    t_in(e, DEV_LC, 0xB0, 25, 40);
    take(e);
    t_in(e, DEV_LC, 0xB0, 25, 127);
    o = take(e);
    CHECK(o.size() == 2, "one knob drives two slots: %s", dump(o).c_str());

    /* ---- pages ---- */
    set(e, "Slot", 4);
    press(e, "Learn");
    t_in(e, DEV_LC, 0x90, 10, 127);   /* pad 2 */
    t_in(e, DEV_LC, 0x80, 10, 0);
    set(e, "Page", 2);                /* page 2 only */
    t_in(e, DEV_LC, 0x90, 10, 127);
    CHECK(take(e).empty(), "page 2 slot silent on page 1");
    t_in(e, DEV_LC, 0x80, 10, 0);
    t_leds(e, led);
    CHECK(led[1] == 0x0C && led[10] == 0x0C && led[11] == 0x3F, "page 1: pad 2 dark, left off, right lit (%02X %02X %02X)", led[1], led[10], led[11]);
    t_in(e, DEV_LC, 0xB0, 117, 127);  /* arrow right */
    t_in(e, DEV_LC, 0xB0, 117, 0);
    CHECK(display(e, "Active Page") == "2", "arrow right -> page 2: '%s'", display(e, "Active Page").c_str());
    t_in(e, DEV_LC, 0x90, 10, 127);
    o = take(e);
    CHECK(is(o, 0, 0x90, 10, 127), "page 2 slot plays: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xB0, 116, 127);  /* arrow left while the note is held */
    o = take(e);
    CHECK(is(o, 0, 0x80, 10, 0), "page change releases the held note: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 10, 0);
    CHECK(take(e).empty(), "no second note off");
    CHECK(display(e, "Active Page") == "1", "back on page 1");
    set(e, "Slot", 4);
    t_in(e, DEV_LC, 0xB0, 115, 127);  /* arrow down: next slot */
    CHECK(display(e, "Slot").rfind("5 ", 0) == 0, "arrow down selects slot 5: '%s'", display(e, "Slot").c_str());
    t_in(e, DEV_LC, 0xB0, 114, 127);
    CHECK(display(e, "Slot").rfind("4 ", 0) == 0, "arrow up selects slot 4");
    set(e, "Arrows", 1);
    t_in(e, DEV_LC, 0xB0, 117, 127);
    CHECK(display(e, "Active Page") == "1", "arrows MAPPABLE: no page change");
    set(e, "Arrows", 0);

    /* PAGE target: pad 3 jumps to page 3, pad 4 = next page */
    set(e, "Slot", 6); press(e, "Learn"); t_in(e, DEV_LC, 0x90, 11, 127); t_in(e, DEV_LC, 0x80, 11, 0);
    set(e, "Type", 4); set(e, "Number", 3);
    CHECK(display(e, "Number") == "PAGE 3", "page target display: '%s'", display(e, "Number").c_str());
    t_in(e, DEV_LC, 0x90, 11, 127); t_in(e, DEV_LC, 0x80, 11, 0);
    CHECK(display(e, "Active Page") == "3", "PAGE target -> page 3: '%s'", display(e, "Active Page").c_str());
    CHECK(take(e).empty(), "PAGE target sends nothing");
    set(e, "Active Page", 0);

    /* PROGRAM target from a UC-33 fader */
    set(e, "Slot", 7); press(e, "Learn");
    t_in(e, DEV_UC, 0xB2, 7, 0);
    CHECK(display(e, "Source") == "UC CH3 CC 7", "UC-33 source: '%s'", display(e, "Source").c_str());
    set(e, "Type", 3); set(e, "Min", 0); set(e, "Max", 7);
    t_in(e, DEV_UC, 0xB2, 7, 127);
    o = take(e);
    CHECK(is(o, 0, 0xC2, 7, 0), "fader -> program 8 (7): %s", dump(o).c_str());
    t_in(e, DEV_USB, 0xB2, 7, 0);
    CHECK(take(e).empty(), "another USB device on the same CC is not the UC-33");

    /* unmapped: drop, then thru */
    t_in(e, DEV_LC, 0xB0, 99, 5);
    CHECK(take(e).empty(), "unmapped dropped");
    set(e, "Unmapped", 1);
    t_in(e, DEV_LC, 0xB0, 99, 5);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 99, 5), "unmapped thru A: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xE0, 0, 64);
    o = take(e);
    CHECK(is(o, 0, 0xE0, 0, 64), "pitch bend thru: %s", dump(o).c_str());
    set(e, "Unmapped", 0);

    /* template change by SysEx: LEDs follow template USER 2 (channel 2) */
    const uint8_t tc[9] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0A, 0x77, 0x01, 0xF7};
    t_sx(e, tc, 9);
    t_leds(e, led);
    CHECK(led[0] == 0x0C, "template 2 has no pad 1 slot (%02X)", led[0]);

    /* CLEAR */
    set(e, "Slot", 7);
    press(e, "Clear");
    CHECK(display(e, "Slot") == "7 (empty)", "clear: '%s'", display(e, "Slot").c_str());
    t_in(e, DEV_UC, 0xB2, 7, 50);
    CHECK(take(e).empty(), "cleared slot silent");

    /* host echo of our own automates must not re-fire or change anything */
    g_echo = true; g_auto_target = e;
    set(e, "Slot", 1);
    t_in(e, DEV_LC, 0xB0, 25, 60);
    block(e); block(e);
    take(e);
    CHECK(display(e, "Slot").rfind("1 ", 0) == 0, "echo keeps slot 1: '%s'", display(e, "Slot").c_str());
    g_echo = false;

    /* ---- chunk: a new instance gets the same map ---- */
    void *ck = nullptr;
    intptr_t n = e->dispatcher(e, 23, 0, 0, &ck, 0);
    CHECK(n > 20 && ck, "chunk");
    std::string chunk((const char *)ck, (size_t)n);
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    e = open_plugin(lib);
    CHECK(display(e, "Slot") == "1 (empty)", "fresh instance empty");
    CHECK(e->dispatcher(e, 24, 0, (intptr_t)chunk.size(), (void *)chunk.data(), 0) == 1, "set chunk");
    CHECK(display(e, "Slot") == "1 LC U1 KNOB 5", "chunk restores slot 1: '%s'", display(e, "Slot").c_str());
    t_in(e, DEV_LC, 0xB0, 25, 127);
    o = take(e);
    CHECK(o.size() == 2, "chunk restores both slots on knob 5: %s", dump(o).c_str());

    /* ---- presets: factory Euclidier map, SAVE / LOAD ---- */
    char nm[64] = {0};
    e->dispatcher(e, 29, 0, 0, nm, 0);
    CHECK(!std::strcmp(nm, "LC > EUCLIDIER FX"), "factory preset 1: '%s'", nm);
    e->dispatcher(e, 2, 0, 0, nullptr, 0);   /* effSetProgram 0 */
    take(e);
    CHECK(display(e, "Link A") == "CONTROL IN", "euclidier map links Out A");
    t_in(e, DEV_LC, 0xB0, 23, 127);           /* knob 3 = lane 3 steps */
    o = take(e);
    CHECK(is(o, 0, 0xBF, 31, 127), "knob 3 -> Euclidier lane 3 steps (CC 31 ch 16): %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xB0, 48, 0);             /* knob 16 = lane 8 fill */
    o = take(e);
    CHECK(is(o, 0, 0xBF, 82, 0), "knob 16 -> lane 8 fill (CC 82): %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x90, 25, 127);           /* pad 5 = lane 5 enable, toggle */
    o = take(e);
    CHECK(is(o, 0, 0xBF, 50, 127), "pad 5 -> lane 5 enable: %s", dump(o).c_str());
    e->dispatcher(e, 2, 0, 1, nullptr, 0);
    take(e);
    t_in(e, DEV_LC, 0xB0, 117, 127);          /* page 2 */
    t_in(e, DEV_LC, 0xB0, 41, 99);
    o = take(e);
    CHECK(is(o, 0, 0xB1, 41, 99), "4 pages map: page 2 = channel 2: %s", dump(o).c_str());
    e->dispatcher(e, 2, 0, 2, nullptr, 0);
    take(e);
    set(e, "Active Page", 3);
    t_in(e, DEV_LC, 0x90, 12, 127);
    o = take(e);
    CHECK(is(o, 3, 0x90, 12, 127), "4 pages > out A-D: page 4 pad 4 = Out D: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 12, 0);
    take(e);
    /* DrumGen on channel 15 */
    e->dispatcher(e, 29, 4, 0, nm, 0);
    CHECK(!std::strcmp(nm, "LC > DRUMGEN (CH 15)"), "factory preset 5: '%s'", nm);
    e->dispatcher(e, 2, 0, 4, nullptr, 0);
    take(e);
    CHECK(display(e, "Link A") == "CONTROL IN", "DrumGen map links Out A");
    set(e, "Active Page", 0);
    t_in(e, DEV_LC, 0xB0, 21, 90);            /* page 1 knob 1 = lane 1 density (CC 13 ch 15) */
    o = take(e);
    CHECK(is(o, 0, 0xBE, 13, 90), "DrumGen page 1 knob 1 -> L1 density: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xB0, 48, 10);            /* knob 16 = lane 8 pattern (CC 82) */
    o = take(e);
    CHECK(is(o, 0, 0xBE, 82, 10), "DrumGen knob 16 -> L8 pattern: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x90, 10, 127);           /* pad 2 = lane 2 on (toggle) */
    o = take(e);
    CHECK(is(o, 0, 0xBE, 20, 127), "DrumGen pad 2 -> L2 on: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 10, 0); take(e);
    set(e, "Active Page", 1);
    t_in(e, DEV_LC, 0x90, 9, 127);            /* page 2 pad 1 = dice lane 1 (momentary) */
    o = take(e);
    CHECK(is(o, 0, 0xBE, 18, 127), "DrumGen dice L1: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 9, 0);
    o = take(e);
    CHECK(is(o, 0, 0xBE, 18, 0), "DrumGen dice released: %s", dump(o).c_str());
    set(e, "Active Page", 2);
    t_in(e, DEV_LC, 0x90, 9, 127);            /* page 3 pad 1 = generate */
    o = take(e);
    CHECK(is(o, 0, 0xBE, 101, 127), "DrumGen generate: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 9, 0); take(e);
    t_in(e, DEV_LC, 0xB0, 23, 64);            /* page 3 knob 3 = swing */
    o = take(e);
    CHECK(is(o, 0, 0xBE, 104, 64), "DrumGen swing: %s", dump(o).c_str());
    set(e, "Active Page", 3);
    t_in(e, DEV_LC, 0x90, 28, 127);           /* page 4 pad 8 = lock lane 8 */
    o = take(e);
    CHECK(is(o, 0, 0xBE, 87, 127), "DrumGen lock L8: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 28, 0); take(e);
    e->dispatcher(e, 2, 0, 2, nullptr, 0);   /* back to "4 pages > Out A-D", page 4, for SAVE / LOAD */
    take(e);
    set(e, "Active Page", 3);

    set(e, "Preset", 10);
    CHECK(display(e, "Preset") == "10 (empty)", "preset 10 empty: '%s'", display(e, "Preset").c_str());
    press(e, "Save");
    CHECK(display(e, "Preset") == "CC Map 10", "saved: '%s'", display(e, "Preset").c_str());
    e->dispatcher(e, 2, 0, 3, nullptr, 0);   /* EMPTY */
    CHECK(display(e, "Slot") == "1 (empty)", "EMPTY clears the map: '%s'", display(e, "Slot").c_str());
    set(e, "Preset", 10);
    press(e, "Load");
    take(e);
    t_in(e, DEV_LC, 0x90, 12, 127);
    o = take(e);
    CHECK(is(o, 3, 0x90, 12, 127), "LOAD restores (page 4 still active): %s", dump(o).c_str());
    t_in(e, DEV_LC, 0x80, 12, 0);
    take(e);
    char newname[24] = "MY LC MAP";
    e->dispatcher(e, 2, 0, 9, nullptr, 0);
    e->dispatcher(e, 4, 0, 0, newname, 0);
    FILE *f = std::fopen(bank, "r");
    CHECK(f != nullptr, "bank file written");
    if (f) {
        char line[256] = {0};
        CHECK(std::fgets(line, sizeof line, f) && !std::strncmp(line, "10\tMY LC MAP\t", 13), "bank line: '%.40s'", line);
        std::fclose(f);
    }
    e->dispatcher(e, 1, 0, 0, nullptr, 0);

    /* ---- 0.2.0: feedback to the EC4, pickup, relative encoders ---- */
    e = open_plugin(lib);
    CHECK(display(e, "Takeover") == "PICKUP", "fresh instance: pickup");
    /* EC4 encoder 1 (CC 1, ch 1) on page 1 (slot 1) and page 2 (slot 2) */
    set(e, "Slot", 1); press(e, "Learn"); t_in(e, DEV_EC4, 0xB0, 1, 20); set(e, "Page", 1);
    CHECK(display(e, "Source") == "EC4 CH1 CC 1", "EC4 source: '%s'", display(e, "Source").c_str());
    set(e, "Slot", 2); press(e, "Learn"); t_in(e, DEV_EC4, 0xB0, 1, 20); set(e, "Page", 2); set(e, "Number", 2);
    take(e);
    t_in(e, DEV_EC4, 0xB0, 1, 30);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 1, 30), "page 1: EC4 drives slot 1 only: %s", dump(o).c_str());
    set(e, "Active Page", 1);
    o = take(e);
    CHECK(is(o, FB + DEV_EC4, 0xB0, 1, 20), "page 2: EC4 gets slot 2's position: %s", dump(o).c_str());
    t_in(e, DEV_EC4, 0xB0, 1, 90);   /* a feedback controller never waits for pickup */
    o = take(e);
    CHECK(is(o, 0, 0xB0, 2, 90), "EC4 page 2 at once: %s", dump(o).c_str());
    set(e, "Active Page", 0);
    o = take(e);
    CHECK(is(o, FB + DEV_EC4, 0xB0, 1, 30), "page 1 again: EC4 back to 30: %s", dump(o).c_str());
    press(e, "Send Values");
    o = take(e);
    CHECK(is(o, FB + DEV_EC4, 0xB0, 1, 30), "SEND VALUES: %s", dump(o).c_str());
    set(e, "Feedback", 0);
    set(e, "Active Page", 1);
    CHECK(take(e).empty(), "feedback OFF: nothing back");
    set(e, "Active Page", 0);
    set(e, "Feedback", 1);
    take(e);

    /* PICKUP: Launch Control knob 1 on page 1 (slot 3) and page 2 (slot 4) */
    set(e, "Slot", 3); press(e, "Learn"); t_in(e, DEV_LC, 0xB0, 21, 10); set(e, "Page", 1);
    t_in(e, DEV_LC, 0xB0, 21, 11);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 21, 11), "next to the learned value: takes over: %s", dump(o).c_str());
    t_in(e, DEV_LC, 0xB0, 21, 100);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 21, 100), "caught: follows: %s", dump(o).c_str());
    set(e, "Active Page", 1);
    set(e, "Slot", 4); press(e, "Learn"); t_in(e, DEV_LC, 0xB0, 21, 12); set(e, "Page", 2); set(e, "Number", 22);
    set(e, "Active Page", 0);   /* knob stands at 12 now, slot 3 at 100 */
    take(e);
    t_in(e, DEV_LC, 0xB0, 21, 13);
    CHECK(take(e).empty(), "pickup: 13 doesn't jump slot 3 (at 100)");
    CHECK(display(e, "Last") == "LC U1 KNOB 1: TO 100", "pickup shows the target: '%s'", display(e, "Last").c_str());
    t_in(e, DEV_LC, 0xB0, 21, 60);
    CHECK(take(e).empty(), "still below");
    t_in(e, DEV_LC, 0xB0, 21, 105);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 21, 105), "crossed 100: takes over: %s", dump(o).c_str());
    set(e, "Active Page", 1);
    take(e);
    t_in(e, DEV_LC, 0xB0, 21, 104);
    CHECK(take(e).empty(), "page 2: slot 4 (at 12) waits");
    t_in(e, DEV_LC, 0xB0, 21, 127);
    o = take(e);
    CHECK(o.size() == 1 && o[0].d1 == 22 && o[0].d2 == 127, "end of travel takes over at once: %s", dump(o).c_str());
    set(e, "Takeover", 0);
    set(e, "Active Page", 0);
    take(e);
    t_in(e, DEV_LC, 0xB0, 21, 40);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 21, 40), "JUMP: at once: %s", dump(o).c_str());
    set(e, "Takeover", 1);

    /* relative encoders */
    set(e, "Slot", 5); press(e, "Learn"); t_in(e, DEV_EC4, 0xB0, 3, 64);
    set(e, "Encoder", 1);
    CHECK(display(e, "Encoder") == "REL 64", "encoder REL 64");
    take(e);
    t_in(e, DEV_EC4, 0xB0, 3, 65); t_in(e, DEV_EC4, 0xB0, 3, 66);
    o = take(e);
    CHECK(o.size() == 2 && o[0].d2 == 65 && o[1].d2 == 67, "REL 64: +1, +2: %s", dump(o).c_str());
    t_in(e, DEV_EC4, 0xB0, 3, 60);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 3, 63), "REL 64: -4: %s", dump(o).c_str());
    set(e, "Encoder", 2);
    t_in(e, DEV_EC4, 0xB0, 3, 127); t_in(e, DEV_EC4, 0xB0, 3, 127);
    o = take(e);
    CHECK(o.size() == 2 && o[1].d2 == 61, "REL 2C: -1, -1: %s", dump(o).c_str());
    for (int i = 0; i < 40; i++) t_in(e, DEV_EC4, 0xB0, 3, 10);
    o = take(e);
    CHECK(!o.empty() && o.back().d2 == 127, "REL clamps at 127: %s", dump(o).c_str());
    press(e, "Send Values");
    o = take(e);
    CHECK(o.size() == 1 && o[0].d1 == 1, "relative slots get no feedback: %s", dump(o).c_str());

    /* positions and encoder modes survive the project, the EC4 is told on load */
    ck = nullptr;
    n = e->dispatcher(e, 23, 0, 0, &ck, 0);
    chunk.assign((const char *)ck, (size_t)n);
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    e = open_plugin(lib);
    e->dispatcher(e, 24, 0, (intptr_t)chunk.size(), (void *)chunk.data(), 0);
    o = take(e);
    CHECK(is(o, FB + DEV_EC4, 0xB0, 1, 30), "load: EC4 gets its position: %s", dump(o).c_str());
    set(e, "Slot", 5);
    CHECK(display(e, "Encoder") == "REL 2C", "encoder mode restored");
    t_in(e, DEV_LC, 0xB0, 21, 90);
    CHECK(take(e).empty(), "pickup position restored (slot 3 at 40)");
    /* a 0.1.0 project (13 fields per slot) still loads */
    const std::string old = "CCMP1;pg=0;fo=1;le=1;th=0;li=0;in=0;ar=0;m1=1,1,0,0,21,0,0,0,1,21,0,127,0;prog=0;sel=0;";
    CHECK(e->dispatcher(e, 24, 0, (intptr_t)old.size(), (void *)old.data(), 0) == 1, "0.1.0 chunk");
    t_in(e, DEV_LC, 0xB0, 21, 77);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 21, 77), "0.1.0 slot plays (position unknown: no pickup): %s", dump(o).c_str());
    e->dispatcher(e, 1, 0, 0, nullptr, 0);

    /* ---- 0.3.0: template colour, number flash, template readout ---- */
    e = open_plugin(lib);
    uint8_t v[12], fl[12];
    t_in(e, DEV_LC, 0xB2, 21, 5);   /* knob from USER 3 */
    CHECK(display(e, "LC Template") == "USER 3", "template readout: '%s'", display(e, "LC Template").c_str());
    CHECK(t_flash(e, fl) == 1, "first sight of the LC: flash");
    CHECK(fl[2] == 0x0F && fl[0] == 0x0C && fl[7] == 0x0C, "flash: pad 3 red alone (%02X %02X)", fl[2], fl[0]);
    t_leds(e, v);
    bool same = true;
    for (int p = 0; p < 8; p++) same &= v[p] == v[0];
    CHECK(same && v[0] == 16 * 2 + 2 + 12, "USER 3, nothing mapped: all pads amber (%02X)", v[0]);
    CHECK(v[8] == 0x0D, "USER: up/down red");
    const uint8_t f2[9] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0A, 0x77, 0x09, 0xF7};
    t_sx(e, f2, 9);
    CHECK(display(e, "LC Template") == "FACTORY 2", "factory readout: '%s'", display(e, "LC Template").c_str());
    CHECK(t_flash(e, fl) == 1 && fl[1] == 0x3C, "flash: pad 2 green for FACTORY 2 (%02X)", fl[1]);
    t_leds(e, v);
    CHECK(v[0] == 16 * 1 + 2 + 12 && v[8] == 0x1C, "FACTORY 2: orange pads, green up/down (%02X %02X)", v[0], v[8]);
    t_sx(e, f2, 9);
    CHECK(t_flash(e, fl) == 0, "same template: no flash");
    uint8_t col[8];
    for (int t = 0; t < 8; t++) {
        const uint8_t sx[9] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0A, 0x77, (uint8_t)t, 0xF7};
        t_sx(e, sx, 9);
        t_leds(e, v);
        col[t] = v[0];
        CHECK((v[0] & 0x03) < 3 && ((v[0] >> 4) & 0x03) < 3, "template colours stay below full brightness");
    }
    bool distinct = true;
    for (int a2 = 0; a2 < 8; a2++) for (int b2 = a2 + 1; b2 < 8; b2++) distinct &= col[a2] != col[b2];
    CHECK(distinct, "8 different template colours");
    /* USER 1 with a toggle pad and a momentary pad: rest = template colour, states = full colours */
    const uint8_t u1[9] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0A, 0x77, 0x00, 0xF7};
    t_sx(e, u1, 9);
    set(e, "Slot", 1); press(e, "Learn"); t_in(e, DEV_LC, 0x90, 9, 127); t_in(e, DEV_LC, 0x80, 9, 0); set(e, "Button", 1);
    set(e, "Slot", 2); press(e, "Learn"); t_in(e, DEV_LC, 0x90, 10, 127); t_in(e, DEV_LC, 0x80, 10, 0);
    take(e);
    t_leds(e, v);
    CHECK(v[0] == 0x0E && v[1] == 0x0E && v[2] == 0x0C, "rest: red template colour on mapped pads, others off (%02X %02X %02X)", v[0], v[1], v[2]);
    t_in(e, DEV_LC, 0x90, 9, 127); t_in(e, DEV_LC, 0x90, 10, 127);
    t_leds(e, v);
    CHECK(v[0] == 0x3C && v[1] == 0x3F, "toggle on green, held amber (%02X %02X)", v[0], v[1]);
    take(e);
    set(e, "LEDs", 0);
    t_leds(e, v);
    CHECK(v[0] == 0x0C && v[8] == 0x0C, "LEDS OFF: dark");
    /* a 0.2.0 project (le=1) keeps the plain slot colours */
    const std::string o2 = "CCMP1;pg=0;fo=1;le=1;th=0;li=0;in=0;ar=0;fb=1;to=1;prog=0;sel=0;";
    e->dispatcher(e, 24, 0, (intptr_t)o2.size(), (void *)o2.data(), 0);
    CHECK(display(e, "LEDs") == "ON", "0.2.0 project: LEDS ON");
    e->dispatcher(e, 1, 0, 0, nullptr, 0);

    /* ---- 0.4.0: OXI E16 ---- */
    e = open_plugin(lib);
    set(e, "Slot", 1); press(e, "Learn"); t_in(e, DEV_E16, 0xB0, 20, 50); set(e, "Page", 1);
    CHECK(display(e, "Source") == "E16 CH1 CC 20", "E16 source: '%s'", display(e, "Source").c_str());
    set(e, "Slot", 2); press(e, "Learn"); t_in(e, DEV_E16, 0xB0, 20, 50); set(e, "Page", 2);
    take(e);
    t_in(e, DEV_E16, 0xB0, 20, 90);
    o = take(e);
    CHECK(is(o, 0, 0xB0, 20, 90), "E16 page 1: %s", dump(o).c_str());
    set(e, "Active Page", 1);
    o = take(e);
    CHECK(is(o, FB + DEV_E16, 0xB0, 20, 50), "page 2: E16 gets slot 2's position: %s", dump(o).c_str());
    t_in(e, DEV_E16, 0xB0, 20, 10);   /* a display controller never waits for pickup */
    o = take(e);
    CHECK(is(o, 0, 0xB0, 20, 10), "E16 no pickup: %s", dump(o).c_str());
    e->dispatcher(e, 1, 0, 0, nullptr, 0);

    /* ---- 0.5.0: phone view, slot names ---- */
    e = open_plugin(lib);
    {
        const std::string page = http_get("/app");
        CHECK(page.find("<title>CC Map</title>") != std::string::npos, "phone page served at /app (%zu bytes)", page.size());
        CHECK(http_get("/").find("<title>Force</title>") != std::string::npos, "hub page at /");
        const std::string info = http_full("/info", hub_port());
        CHECK(info.find("Access-Control-Allow-Origin: *") != std::string::npos, "info: CORS header");
        CHECK(info.find("\"id\":\"ccmap\"") != std::string::npos && info.find("\"app\":\"/app\"") != std::string::npos, "info: %s", info.c_str());
        char pp[32]; std::snprintf(pp, sizeof pp, "\"port\":%d", hub_port());
        CHECK(info.find(pp) != std::string::npos, "info: on the first port of the range");
        CHECK(info.find("kein Launch Control") != std::string::npos, "info: summary");
        /* a second instance takes the next port; its hub sees the same page */
        AEffect *e2 = open_plugin(lib);
        CHECK(http_get("/info", hub_port() + 1).find("\"name\":\"Baukasten\"") != std::string::npos, "0.8.0: the kit is the second tile");
        CHECK(http_get("/info", hub_port() + 2).empty(), "second instance is no owner: no further server");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
        /* another plugin with a hub (here: one in the test itself) takes the next free port */
        forcehub::Server other;
        other.id = "matrix"; other.name = "Matrix"; other.app = "<title>M</title>";
        other.summary = [] { return std::string("A1 · PLAY"); };
        CHECK(other.start() && other.port == hub_port() + 2, "another plugin on the next free port (%d)", other.port);
        CHECK(http_get("/info", other.port).find("\"name\":\"Matrix\"") != std::string::npos, "second plugin's info");
        CHECK(http_get("/app", other.port) == "<title>M</title>", "second plugin's app");
        CHECK(http_get("/state", other.port).find("not found") != std::string::npos, "routes stay with their plugin");
        other.stop();
        set(e, "Slot", 1); press(e, "Learn"); t_in(e, DEV_LC, 0xB0, 21, 30); set(e, "Number", 74); set(e, "Channel", 5);
        set(e, "Slot", 2); press(e, "Learn"); t_in(e, DEV_E16, 0xB0, 7, 99);
        take(e);
        std::string js = http_get("/state");
        CHECK(js.find("\"tmpl\":\"USER 1\"") != std::string::npos, "state: template: %.80s", js.c_str());
        CHECK(js.find("\"target\":\"CC 74 / CH 5 / A\"") != std::string::npos, "state: knob 1 target");
        CHECK(js.find("\"src\":\"E16 CH1 CC 7\"") != std::string::npos, "state: E16 in others");
        CHECK(http_get("/label?slot=1&name=Clem%20Cutoff").find("ok") != std::string::npos, "label set");
        set(e, "Slot", 1);
        CHECK(display(e, "Slot") == "1 Clem Cutoff", "slot shows the name: '%s'", display(e, "Slot").c_str());
        js = http_get("/state");
        CHECK(js.find("\"label\":\"Clem Cutoff\"") != std::string::npos, "state carries the name");
        t_in(e, DEV_LC, 0xB0, 21, 31);
        take(e);
        CHECK(display(e, "Last") == "Clem Cutoff = 31", "LAST shows the name: '%s'", display(e, "Last").c_str());
        js = http_get("/state");
        CHECK(js.find("\"last_slot\":0") != std::string::npos, "last slot highlighted");
        http_get("/label?slot=2&name=A%3Bb%3Dc%25");   /* separators inside a name survive the state text */
        CHECK(http_get("/label?slot=999&name=x").find("slot") != std::string::npos, "bad slot refused");
        CHECK(http_get("/nothing").find("not found") != std::string::npos, "404");
        CHECK(display(e, "Phone Address").find(":18080") != std::string::npos, "address shown: '%s'", display(e, "Phone Address").c_str());
        void *ck2 = nullptr;
        intptr_t n2 = e->dispatcher(e, 23, 0, 0, &ck2, 0);
        std::string c2((const char *)ck2, (size_t)n2);
        set(e, "Web View", 0);
        CHECK(display(e, "Phone Address") == "OFF", "web view off");
        CHECK(http_get("/state").empty(), "server stopped");
        e->dispatcher(e, 1, 0, 0, nullptr, 0);
        e = open_plugin(lib);
        e->dispatcher(e, 24, 0, (intptr_t)c2.size(), (void *)c2.data(), 0);
        set(e, "Slot", 2);
        CHECK(display(e, "Slot") == "2 A;b=c%", "name with separators restored: '%s'", display(e, "Slot").c_str());
        CHECK(http_get("/state").find("Clem Cutoff") != std::string::npos, "restored project serves its names");
        e->dispatcher(e, 1, 0, 0, nullptr, 0);
    }

    /* ---- 0.6.0: knobs and pads from the phone, stems download ---- */
    {
        char stems[] = "/tmp/ccmap_stems_XXXXXX";
        CHECK(mkdtemp(stems) != nullptr, "temp stems dir");
        setenv("CCMAP_STEMS", stems, 1);
        const std::string song = std::string(stems) + "/Song 1";
        mkdir(song.c_str(), 0755);
        std::string wav1 = song + "/Kick.wav", wav2 = song + "/Bass.wav", txt = song + "/notes.txt";
        std::string data1(100000, 'k'), data2 = "RIFF-bass";
        for (size_t i = 0; i < data1.size(); i++) data1[i] = (char)(i * 7);
        FILE *f = std::fopen(wav1.c_str(), "wb"); std::fwrite(data1.data(), 1, data1.size(), f); std::fclose(f);
        f = std::fopen(wav2.c_str(), "wb"); std::fwrite(data2.data(), 1, data2.size(), f); std::fclose(f);
        f = std::fopen(txt.c_str(), "wb"); std::fputs("secret", f); std::fclose(f);

        e = open_plugin(lib);
        set(e, "Slot", 1); press(e, "Learn"); t_in(e, DEV_E16, 0xB0, 20, 10); set(e, "Number", 74);
        set(e, "Slot", 2); press(e, "Learn"); t_in(e, DEV_LC, 0x90, 9, 127); t_in(e, DEV_LC, 0x80, 9, 0); set(e, "Type", 1); set(e, "Number", 60); set(e, "Button", 1);
        set(e, "Slot", 3); press(e, "Learn"); t_in(e, DEV_LC, 0xB0, 22, 10);
        take(e);
        CHECK(http_get("/set?slot=1&val=99").find("ok") != std::string::npos, "remote set");
        o = take(e);
        CHECK(o.size() == 2 && o[0].port == FB + DEV_E16 && o[0].d2 == 99 && o[1].port == 0 && o[1].d1 == 74 && o[1].d2 == 99,
              "remote knob: E16 told + Out A CC 74 = 99: %s", dump(o).c_str());
        http_get("/press?slot=2&down=1"); http_get("/press?slot=2&down=0");
        o = take(e);
        CHECK(is(o, 0, 0xB0, 60, 127), "remote pad toggles on: %s", dump(o).c_str());
        CHECK(http_get("/set?slot=50&val=1").find("no such") != std::string::npos, "remote: empty slot refused");
        http_get("/set?slot=3&val=100");
        take(e);
        t_in(e, DEV_LC, 0xB0, 22, 11);   /* the pot still stands at 11: picks up, doesn't jump */
        CHECK(take(e).empty(), "after a remote move the LC pot must pick up again");
        CHECK(display(e, "Last") == "LC U1 KNOB 2: TO 100", "pickup target after remote: '%s'", display(e, "Last").c_str());

        CHECK(http_get("/files").find("off") != std::string::npos, "files off by default");
        set(e, "Files", 1);
        std::string j = http_get("/files");
        CHECK(j.find("\"Song 1\"") != std::string::npos, "single Stems folder opens directly: %.120s", j.c_str());
        j = http_get(("/files?dir=" + song).c_str());
        CHECK(j.find("Kick.wav") != std::string::npos && j.find("Bass.wav") != std::string::npos, "audio files listed");
        CHECK(j.find("notes.txt") == std::string::npos, "other files hidden");
        CHECK(http_get(("/file?path=" + song + "/Kick.wav").c_str()) == data1, "file download intact");
        CHECK(http_get(("/file?path=" + txt).c_str()).find("not available") != std::string::npos, "non-audio refused");
        CHECK(http_get("/file?path=/etc/passwd").find("not available") != std::string::npos, "outside Stems refused");
        CHECK(http_get(("/files?dir=" + song + "/../..").c_str()).find("not allowed") != std::string::npos, "../ out of Stems refused");
        std::string z = http_get(("/zip?dir=" + song).c_str());
        const std::string zp = std::string(stems) + ".zip";
        f = std::fopen(zp.c_str(), "wb"); std::fwrite(z.data(), 1, z.size(), f); std::fclose(f);
        CHECK(z.size() > data1.size() + data2.size() && z.compare(0, 4, "PK\x03\x04") == 0 && z.find("PK\x05\x06") != std::string::npos,
              "zip streamed (%zu bytes)", z.size());
        std::printf("ZIP %s\n", zp.c_str());
        set(e, "Files", 0);
        CHECK(http_get("/files").find("off") != std::string::npos, "files off again");
        e->dispatcher(e, 1, 0, 0, nullptr, 0);
        unsetenv("CCMAP_STEMS");
    }

    /* ---- 0.8.0: BAUKASTEN ---- */
    {
        e = open_plugin(lib);
        const int kp = hub_port() + 1;
        auto kget = [&](const std::string &path) { return http_get(path.c_str(), kp); };
        CHECK(kget("/app").find("<title>Baukasten</title>") != std::string::npos, "kit page served");
        CHECK(kget("/info").find("Eigene Regler bauen") != std::string::npos, "empty kit summary: %s", kget("/info").c_str());
        CHECK(kget("/kit").find("\"pages\":[]") != std::string::npos, "no pages yet");
        CHECK(kget("/el?op=put&p=0&i=-1&t=fader").find("no such page") != std::string::npos, "element without page refused");
        std::string j = kget("/page?op=add&name=Clem%20XT&cols=4");
        CHECK(j.find("\"name\":\"Clem XT\"") != std::string::npos, "page added: %s", j.c_str());
        j = kget("/el?op=put&p=0&i=-1&t=fader&n=Cutoff&c=2&o=2&ch=5&cc=74&lo=0&hi=127&w=1&h=3");
        CHECK(j.find("\"n\":\"Cutoff\"") != std::string::npos && j.find("\"ch\":5") != std::string::npos, "fader added: %s", j.c_str());
        kget("/el?op=put&p=0&i=-1&t=button&n=Fill&o=0&ch=16&cc=20&lo=0&hi=100");
        kget("/el?op=put&p=0&i=-1&t=xy&n=Filter%20XY&o=1&ch=1&cc=71&cc2=74&lo=127&hi=0");
        take(e);
        CHECK(kget("/send?p=0&i=0&v=127").find("ok") != std::string::npos, "send ok");
        o = take(e);
        CHECK(is(o, 2, 0xB4, 74, 127), "fader -> Out C ch 5 CC 74 = 127: %s", dump(o).c_str());
        kget("/send?p=0&i=0&v=127");
        CHECK(take(e).empty(), "same fader value not sent twice");
        CHECK(display(e, "Last") == "Cutoff = 127", "Force shows the kit move: '%s'", display(e, "Last").c_str());
        kget("/send?p=0&i=1&v=127"); kget("/send?p=0&i=1&v=0"); kget("/send?p=0&i=1&v=0");
        o = take(e);
        CHECK(o.size() == 3 && o[0].port == 0 && o[0].st == 0xBF && o[0].d1 == 20 && o[0].d2 == 100 && o[1].d2 == 0 && o[2].d2 == 0,
              "button: MAX on press, MIN on release, always sent: %s", dump(o).c_str());
        kget("/send?p=0&i=2&v=0&v2=127");
        o = take(e);
        CHECK(o.size() == 2 && o[0].port == 1 && o[0].st == 0xB0 && o[0].d1 == 71 && o[0].d2 == 127 && o[1].d1 == 74 && o[1].d2 == 0,
              "xy inverted, two CCs on Out B: %s", dump(o).c_str());
        CHECK(kget("/vals?p=0").find("[127,-1],[0,-1],[0,127]") != std::string::npos, "values: %s", kget("/vals?p=0").c_str());
        CHECK(kget("/send?p=0&i=9&v=1").find("no such") != std::string::npos, "send to a missing element refused");
        /* edits: retarget, clamp, move, duplicate, delete */
        j = kget("/el?op=put&p=0&i=0&ch=99&cc=300&w=0&n=" + std::string(40, 'x'));
        CHECK(j.find("\"ch\":16") != std::string::npos && j.find("\"cc\":127") != std::string::npos && j.find("\"w\":1") != std::string::npos &&
              j.find(std::string(24, 'x') + "\"") != std::string::npos && j.find(std::string(25, 'x')) == std::string::npos, "values clamped: %s", j.c_str());
        kget("/el?op=put&p=0&i=0&n=Cutoff&ch=5&cc=74");
        kget("/send?p=0&i=0&v=127");
        CHECK(is(take(e), 2, 0xB4, 74, 127), "a retargeted element sends its next move again");
        j = kget("/el?op=move&p=0&i=2&dir=-1");
        CHECK(j.find("Filter XY") < j.find("\"Fill\""), "moved forward");
        kget("/el?op=dup&p=0&i=0");
        kget("/el?op=del&p=0&i=3");
        j = kget("/kit");
        CHECK(j.find("Cutoff") != j.rfind("Cutoff") && j.find("Filter XY") != std::string::npos && j.find("\"Fill\"") == std::string::npos, "dup + del: %s", j.c_str());
        CHECK(kget("/el?op=nope&p=0&i=0").find("op:") != std::string::npos, "unknown op refused");
        /* 0.9.0: PICKER - sends the chosen value itself, names for the values, step */
        j = kget("/el?op=put&p=0&i=-1&t=picker&n=Wave&o=3&ch=2&cc=30&lo=0&hi=48&st=16&lb=Sinus,S%C3%A4ge,%20Rechteck%20,Rauschen");
        CHECK(j.find("\"t\":\"picker\"") != std::string::npos && j.find("\"st\":16") != std::string::npos &&
              j.find("\"lb\":\"Sinus,S\xC3\xA4ge, Rechteck ,Rauschen\"") != std::string::npos, "picker added: %s", j.c_str());
        const int pk = 3;   /* Cutoff, Cutoff, Filter XY, Wave */
        kget("/send?p=0&i=" + std::to_string(pk) + "&v=32");
        o = take(e);
        CHECK(is(o, 3, 0xB1, 30, 32), "picker sends 32 itself on Out D ch 2 CC 30: %s", dump(o).c_str());
        CHECK(display(e, "Last") == "Wave = Rechteck (32)", "Force shows the value's name: '%s'", display(e, "Last").c_str());
        kget("/send?p=0&i=" + std::to_string(pk) + "&v=40");
        take(e);
        CHECK(display(e, "Last") == "Wave = 40", "a value without a name: '%s'", display(e, "Last").c_str());
        CHECK(kget("/el?op=put&p=0&i=3&st=0").find("\"st\":1") != std::string::npos, "step at least 1");
        kget("/el?op=put&p=0&i=3&st=16");
        /* umlauts and separators in names, a second page, page order */
        kget("/page?op=add&name=Gr%C3%BC%C3%9Fe%09%0Atab&cols=12");
        j = kget("/kit");
        CHECK(j.find("\"name\":\"Gr\xC3\xBC\xC3\x9F" "etab\",\"cols\":8") != std::string::npos, "page name cleaned, cols clamped: %s", j.c_str());
        kget("/page?op=move&p=1&dir=-1");
        j = kget("/kit");
        CHECK(j.find("Gr\xC3\xBC") < j.find("Clem XT"), "page moved to the front");
        CHECK(kget("/info").find("2 Seiten") != std::string::npos, "kit summary: %s", kget("/info").c_str());
        /* the file: written on edits, read by the next owner */
        std::string txt;
        if (FILE *f = std::fopen(kitf, "r")) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) txt.append(b, n); std::fclose(f); }
        CHECK(txt.compare(0, 7, "CCKIT1\n") == 0 && txt.find("E\tfader\tCutoff\t2\t2\t5\t74\t") != std::string::npos, "kit file: %s", txt.c_str());
        CHECK(txt.find("E\tpicker\tWave\t0\t3\t2\t30\t2\t0\t48\t1\t2\t16\tSinus,S\xC3\xA4ge, Rechteck ,Rauschen\n") != std::string::npos, "picker in the file: %s", txt.c_str());
        /* import from the map: knob (CC), pad (TOGGLE), a NOTE slot is skipped */
        set(e, "Slot", 1); press(e, "Learn"); t_in(e, DEV_LC, 0xB0, 21, 40); set(e, "Number", 74); set(e, "Channel", 3);
        http_get("/label?slot=1&name=Res");
        set(e, "Slot", 2); press(e, "Learn"); t_in(e, DEV_LC, 0x90, 9, 127); t_in(e, DEV_LC, 0x80, 9, 0); set(e, "Button", 1);
        set(e, "Type", 1); set(e, "Number", 30);
        set(e, "Slot", 3); press(e, "Learn"); t_in(e, DEV_LC, 0x90, 10, 127); t_in(e, DEV_LC, 0x80, 10, 0);   /* NOTE target */
        take(e);
        j = kget("/import");
        CHECK(j.find("CC Map S1") != std::string::npos && j.find("{\"t\":\"knob\",\"c\":2,\"o\":0,\"ch\":3,\"cc\":74") != std::string::npos &&
              j.find("\"t\":\"toggle\"") != std::string::npos && j.find("\"n\":\"Res\"") != std::string::npos, "imported: %s", j.c_str());
        CHECK(j.find("LC U1 PAD 2") == std::string::npos, "NOTE slot not imported");
        e->dispatcher(e, 1, 0, 0, nullptr, 0);
        e = open_plugin(lib);
        j = http_get("/kit", kp);
        CHECK(j.find("Cutoff") != std::string::npos && j.find("CC Map S1") != std::string::npos && j.find("\"v\":-1") != std::string::npos,
              "kit restored from the file, values unknown: %s", j.c_str());
        CHECK(j.find("\"lb\":\"Sinus,S\xC3\xA4ge") != std::string::npos, "picker names restored");
        e->dispatcher(e, 1, 0, 0, nullptr, 0);
        if (FILE *f = std::fopen(kitf, "w")) { std::fputs("CCKIT1\nP\tOld\t4\nE\tknob\tCut\t1\t0\t5\t74\t75\t0\t127\t1\t2\n", f); std::fclose(f); }
        e = open_plugin(lib);
        j = kget("/kit");
        CHECK(j.find("\"n\":\"Cut\"") != std::string::npos && j.find("\"st\":1") != std::string::npos && j.find("\"lb\":\"\"") != std::string::npos,
              "a 0.8.0 kit file loads: %s", j.c_str());
        CHECK(kget("/page?op=del&p=9").find("no such page") != std::string::npos, "delete missing page refused");
        for (int k = 0; k < 20; k++) kget("/page?op=add");
        CHECK(kget("/page?op=add").find("16 pages") != std::string::npos, "at most 16 pages");
        for (int k = 0; k < 50; k++) kget("/el?op=put&p=5&i=-1&t=knob");
        CHECK(kget("/el?op=dup&p=5&i=0").find("48 elements") != std::string::npos, "at most 48 elements");
        set(e, "Web View", 0);
        CHECK(kget("/kit").empty(), "kit server stops with the phone view");
        set(e, "Web View", 1);
        CHECK(kget("/kit").find("\"name\":\"Old\"") != std::string::npos, "and comes back");
        e->dispatcher(e, 1, 0, 0, nullptr, 0);
        unlink(kitf);
    }

    /* ---- stress: random parameters and MIDI ---- */
    e = open_plugin(lib);
    srand(7);
    for (int it = 0; it < 20000; it++) {
        const int r = rand() % 10;
        if (r < 3) e->setParameter(e, rand() % e->numParams, (rand() % 1001) / 1000.0f);
        else if (r < 9) {
            static const int ST[5] = {0x90, 0x80, 0xB0, 0xE0, 0xC0};
            t_in(e, 1 + rand() % 5, ST[rand() % 5] | (rand() % 16), rand() % 128, rand() % 128);
        } else block(e);
        if (it % 50 == 0) { take(e); char b[64]; for (int i = 0; i < e->numParams; i++) e->dispatcher(e, 7, i, 0, b, 0); }
        if (it % 997 == 0) e->dispatcher(e, 2, 0, rand() % 32, nullptr, 0);
    }
    take(e);
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    unlink(bank);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
