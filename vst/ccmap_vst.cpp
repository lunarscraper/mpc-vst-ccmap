/* =============================================================================
 * ccmap_vst.cpp - CC Map: a MIDI controller mapper as a VST2 insert effect for the MPC OS plugin
 * host (Force, MPC Live/One/X/Key), armhf.
 *
 * Why an insert effect: it takes none of the 8 plugin instrument slots, and the track's audio
 * passes through untouched. Put it on any track (or the master); it works as long as the project
 * has it loaded.
 *
 * MIDI path (MPC OS ignores VST MIDI out, mpc-vst-plugins docs/NOTES.md; pattern of Euclidier FX):
 *
 *   Launch Control / UC-33 / any USB controller
 *        |  own ALSA seq client "CC Map", hidden input port: it subscribes itself to every
 *        v  hardware MIDI input (also ones plugged in later) and tells the devices apart by name
 *   ccmap_core.h  (learn, 128 slots, 4 pages, CC / NOTE / PROGRAM / PAGE, MIN..MAX, TOGGLE)
 *        |
 *        +--> ports "Out A".."Out D": choose them as MIDI input of a track (Preferences -> MIDI:
 *        |    enable Track on them first), or as the output port of a MIDI track
 *        +--> "Link A": Out A also feeds every other plugin's "Control In" port (Euclidier FX,
 *        |    Acid FX, Maze FX ...), no routing needed
 *        +--> Launch Control LEDs over SysEx (pads show their slot, arrows show the pages)
 *        +--> PHONE VIEW: a web page on the shared hub (forcehub.h, port 8000..8009) of the Force's WLAN (ccmap_web.h): the Launch Control with
 *             the names, targets and positions of the active template, other controllers below; tap = name,
 *             drag a knob / tap a pad = play it from the phone; FILES: download the Stems folder
 *        +--> FEEDBACK: the slots' positions back to a Faderfox EC4 / OXI E16 (value display, no jumps) on a
 *             page change, a load and when it is plugged in; PICKUP for pots without feedback
 *
 *   BAUKASTEN (ccmap_kit.h, page/kit.html): pages of faders, knobs, buttons, toggles and XY pads the
 *   user builds on the phone; every element sends its own CC on its own port and channel (Out A..D).
 *   A second hub server of the same plugin, so the hub shows it as a tile of its own.
 *
 * All ALSA traffic runs on one thread of its own (poll(), no dependence on the audio callback, so
 * mapping also works while the host doesn't call an idle insert). The host's thread and that thread
 * share the mapper under Plugin::mu; outputs are queued in the mapper and only that thread sends.
 * Parameter changes the plugin makes itself (LEARN done, FOLLOW, page keys) are reported to the
 * host from processReplacing (never from inside the host's own call, docs/NOTES.md).
 *
 * ONE instance per project drives the controllers; a second one loads but stays passive (logged
 * to /tmp/ccmap_vst.log), so nothing gets sent twice.
 * MIT license (see ../LICENSE). "Novation", "Launch Control" and "Evolution UC-33" belong to their
 * owners; no affiliation.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "ccmap_core.h"
#include "ccmap_web.h"
#include "ccmap_page.h"   /* page/index.html, embedded by gen_page.py */
#include "ccmap_kit.h"
#include "ccmap_kit_page.h"   /* page/kit.html (BAUKASTEN), embedded by gen_page.py */

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
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
enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetProgramNameIndexed = 29, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterUpdateDisplay = 42 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };

#define BUILD_ID "ccmap-0.10.0"
static FILE *g_log;
static std::mutex g_log_lock;
#define LOG(...) do { std::lock_guard<std::mutex> lg_(g_log_lock); if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

using namespace ccmap;

/* the parameters, by key (module.json); IDX[] is their position in the generated PARAMS[] */
enum {
    SLOT, LEARN, SOURCE, PAGE, OUT, CH, TYPE, NUM, LO, HI, BTN, CLEAR,
    CUR_PAGE, FOLLOW, LEDS, THRU, LINK, INPUT, ARROWS, MONITOR, PRESET, LOAD, SAVE,
    /* 0.2.0, appended */
    ENC, FEEDBACK, TAKEOVER, SEND,
    /* 0.3.0 */
    TEMPLATE,
    /* 0.5.0 */
    WEB, WEB_ADDR,
    /* 0.6.0 */
    FILES, NKEYS
};
static const char *const KEYS[NKEYS] = {
    "slot", "learn", "source", "page", "out", "ch", "type", "num", "lo", "hi", "btn", "clear",
    "cur_page", "follow", "leds", "thru", "link", "input", "arrows", "monitor", "preset", "load", "save",
    "enc", "feedback", "takeover", "send", "template", "web", "web_addr", "files",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours (popup flags) */
static bool is_trigger(int k) { return k == LEARN || k == CLEAR || k == LOAD || k == SAVE || k == SEND; }

#define NSLOTS 32

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::mutex mu;                       /* guards mp */
    Mapper mp;
    bool owner = false;
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};        /* CLEAR/LOAD/SAVE: the host currently reports them pressed */
    std::atomic<uint64_t> pending{0};    /* params whose value changed from our side */
    std::atomic<bool> redisplay{false};
    std::atomic<int> cur{0};             /* selected preset slot, 0-based */
    std::vector<uint8_t> chunk;
    forcehub::Server *web = nullptr;     /* phone view on the shared hub, owner instance only */
    ccweb::Hooks hooks;
    cckit::Kit kit;                      /* BAUKASTEN, guarded by mu */
    forcehub::Server *kitweb = nullptr;  /* its own tile on the hub, owner instance only */
    /* diagnostics */
    std::atomic<long> n_in{0}, n_out{0}, n_ignored{0}, n_fb{0};
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int in_port = -1, led_port = -1, out_port[NOUTS] = {-1, -1, -1, -1};
    int ret_port = -1;                   /* "CC Map Return": CCs coming back move the BAUKASTEN elements */
    int wake[2] = {-1, -1};
    std::thread thr;
    std::atomic<bool> running{false};
    std::map<int, int> dev_of;           /* ALSA client -> Dev, -1 = ignored (thread only) */
    int lc_client = -1, lc_port = -1;    /* where the Launch Control's LEDs are (thread only) */
    std::map<int, int> fb_port;          /* client -> its writable port, for FEEDBACK (thread only) */
    uint8_t led_sent[NLEDS];
    int led_tmpl = -1;
    double flash_until = 0;              /* template number on the pads until then (thread only) */
    std::vector<std::pair<int, int>> linked;
    int link_applied = 0;
#else
    std::vector<Msg> test_out;           /* offline test: what would have gone out */
#endif
};
static std::atomic<Plugin *> g_owner{nullptr};

/* ---- helpers --------------------------------------------------------------- */
static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
static void mark(Plugin *w, int k) { if (IDX[k] >= 0 && !is_trigger(k)) w->pending.fetch_or(1ull << IDX[k]); }
static void mark_editor(Plugin *w) { for (int k = SLOT; k <= BTN; k++) mark(w, k); mark(w, ENC); }
static void mark_all(Plugin *w) { for (int k = 0; k < NKEYS; k++) mark(w, k); }
/* caller holds w->mu: turn the mapper's change bits into host notifications */
static void collect(Plugin *w) {
    const int c = w->mp.changed;
    w->mp.changed = 0;
    if (c & (CH_SEL | CH_SLOT | CH_LEARN)) mark_editor(w);
    if (c & CH_PAGE) mark(w, CUR_PAGE);
    if (c) w->redisplay.store(true);
}

/* the value of key k in the units of module.json; caller holds w->mu */
static int ui_value(Plugin *w, int k) {
    const Mapper &mp = w->mp;
    const Map &m = mp.map[mp.sel];
    switch (k) {
    case SLOT: return mp.sel + 1;
    case PAGE: return m.page;
    case OUT: return m.out;
    case CH: return m.ch + 1;
    case TYPE: return m.type;
    case NUM: return m.num;
    case LO: return m.lo;
    case HI: return m.hi;
    case BTN: return m.btn;
    case CUR_PAGE: return mp.s.page;
    case FOLLOW: return mp.s.follow;
    case LEDS: return mp.s.leds;
    case THRU: return mp.s.thru;
    case LINK: return mp.s.link;
    case INPUT: return mp.s.input;
    case ARROWS: return mp.s.arrows;
    case ENC: return m.enc;
    case FEEDBACK: return mp.s.feedback;
    case TAKEOVER: return mp.s.takeover;
    case WEB: return mp.s.web;
    case FILES: return mp.s.files;
    case PRESET: return w->cur.load() + 1;
    default: return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Preset bank (pattern of mpc-vst-rattler / mpc-vst-acid): NSLOTS presets shared by every
 * instance and every project, kept in one text file on the SD card (one line per saved preset:
 * "index<TAB>name<TAB>state"). They are also the plugin's VST programs, so the host's PRESET
 * list shows and selects them; the PRESET knob + LOAD/SAVE do the same from the skin. The first
 * presets are factory maps until SAVE overwrites them (a factory map is never written to the file).
 * ------------------------------------------------------------------------- */
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NSLOTS], g_slot_name[NSLOTS], g_bank_path;

/* factory maps, built with the mapper itself (Launch Control template USER 1 = channel 1) */
static Map lc_src(int stype, int num, int page) {
    Map m;
    m.used = true; m.dev = DEV_LC; m.sch = 0; m.stype = (uint8_t)stype; m.snum = (uint8_t)num; m.page = (uint8_t)page;
    return m;
}
static std::string factory_euclidier() {
    /* Euclidier FX's CC table: lane L = CC 10*L + (0 enable, 1 steps, 2 fill), control channel 16 */
    Mapper f;
    f.s.link = 1;
    int k = 0;
    for (int L = 1; L <= 8; L++) {
        Map a = lc_src(S_CC, 20 + L, 0);
        a.type = D_CC; a.ch = 15; a.num = (uint8_t)(10 * L + 1);
        Map b = lc_src(S_CC, 40 + L, 0);
        b.type = D_CC; b.ch = 15; b.num = (uint8_t)(10 * L + 2);
        Map p = lc_src(S_NOTE, L <= 4 ? 8 + L : 20 + L, 0);
        p.type = D_CC; p.ch = 15; p.num = (uint8_t)(10 * L); p.btn = B_TOGGLE;
        std::snprintf(a.name, sizeof a.name, "L%d Steps", L);
        std::snprintf(b.name, sizeof b.name, "L%d Fill", L);
        std::snprintf(p.name, sizeof p.name, "Lane %d", L);
        f.map[k++] = a; f.map[k++] = b; f.map[k++] = p;
    }
    return f.state();
}
static std::string factory_pages(bool by_port) {
    /* page n: the 16 knobs and 8 pads as they are, on channel n (by_port: on Out A..D, channel 1) */
    Mapper f;
    int k = 0;
    for (int pg = 1; pg <= NPAGES; pg++) {
        for (int i = 0; i < 16; i++) {
            const int cc = i < 8 ? 21 + i : 41 + i - 8;
            Map m = lc_src(S_CC, cc, pg);
            m.type = D_CC; m.num = (uint8_t)cc;
            m.out = (uint8_t)(by_port ? pg - 1 : 0); m.ch = (uint8_t)(by_port ? 0 : pg - 1);
            f.map[k++] = m;
        }
        for (int i = 0; i < 8; i++) {
            const int note = i < 4 ? 9 + i : 25 + i - 4;
            Map m = lc_src(S_NOTE, note, pg);
            m.type = D_NOTE; m.num = (uint8_t)note;
            m.out = (uint8_t)(by_port ? pg - 1 : 0); m.ch = (uint8_t)(by_port ? 0 : pg - 1);
            f.map[k++] = m;
        }
    }
    return f.state();
}
/* DrumGen / DrumGen FX (mpc-vst-drumgen 1.1.0) on CONTROL CH 15: lane L = CC 10*L + (0 on, 2 pattern, 3 density,
 * 4 length, 5 note, 7 lock, 8 dice), 100 style, 101 generate, 102 variate, 103 random, 104 swing, 105 gate, 106 auto,
 * 108 load, FX 90 remix on, 92 type, 93 every, 95 echo on, 96 time, 97 repeats, 98 probability, 99 falloff, 111 glitch
 * on, 112 repeats, 113 gate, 114 probability, 115 random, 107 preset. Channel 15, not DrumGen's default 16: the Euclidier map (preset 1) uses
 * 16 and the same CC numbers.
 *   page 1 PLAY    density 1-8 / pattern 1-8, pads lane on/off
 *   page 2 LANES   length 1-8 / note 1-8, pads dice
 *   page 3 GLOBAL  style, random, swing, gate, auto, remix type, remix every, echo time /
 *                  echo repeats, echo prob, echo falloff, glitch repeats, glitch gate, glitch prob, glitch random,
 *                  preset; pads generate, variate, remix on, echo on, glitch on, load preset
 *   page 4 LOCK    pads lock 1-8 (knobs free for LEARN) */
static std::string factory_drumgen() {
    Mapper f;
    f.s.link = 1;
    int k = 0;
    auto knob = [&](int pg, int i, int cc, const char *name) {   /* i 0..15: top row 0..7, bottom row 8..15 */
        Map m = lc_src(S_CC, i < 8 ? 21 + i : 41 + i - 8, pg);
        m.type = D_CC; m.ch = 14; m.num = (uint8_t)cc;
        std::snprintf(m.name, sizeof m.name, "%s", name);
        f.map[k++] = m;
    };
    auto pad = [&](int pg, int i, int cc, bool toggle, const char *name) {
        Map m = lc_src(S_NOTE, i < 4 ? 9 + i : 25 + i - 4, pg);
        m.type = D_CC; m.ch = 14; m.num = (uint8_t)cc; m.btn = toggle ? B_TOGGLE : B_MOMENT;
        std::snprintf(m.name, sizeof m.name, "%s", name);
        f.map[k++] = m;
    };
    char nm[24];
    for (int L = 1; L <= 8; L++) {
        std::snprintf(nm, sizeof nm, "L%d Density", L); knob(1, L - 1, 10 * L + 3, nm);
        std::snprintf(nm, sizeof nm, "L%d Pattern", L); knob(1, 7 + L, 10 * L + 2, nm);
        std::snprintf(nm, sizeof nm, "L%d On", L); pad(1, L - 1, 10 * L, true, nm);
        std::snprintf(nm, sizeof nm, "L%d Length", L); knob(2, L - 1, 10 * L + 4, nm);
        std::snprintf(nm, sizeof nm, "L%d Note", L); knob(2, 7 + L, 10 * L + 5, nm);
        std::snprintf(nm, sizeof nm, "L%d Dice", L); pad(2, L - 1, 10 * L + 8, false, nm);
        std::snprintf(nm, sizeof nm, "L%d Lock", L); pad(4, L - 1, 10 * L + 7, true, nm);
    }
    static const struct { int cc; const char *name; } G[16] = {
        {100, "Style"}, {103, "Random"}, {104, "Swing"}, {105, "Gate"}, {106, "Auto"}, {92, "Remix Type"}, {93, "Remix Every"}, {96, "Echo Time"},
        {97, "Echo Repeats"}, {98, "Echo Prob"}, {99, "Echo Falloff"}, {112, "Glitch Repeats"}, {113, "Glitch Gate"}, {114, "Glitch Prob"}, {115, "Glitch Random"}, {107, "Preset"}};
    for (int i = 0; i < 16; i++) knob(3, i, G[i].cc, G[i].name);
    pad(3, 0, 101, false, "Generate");
    pad(3, 1, 102, false, "Variate");
    pad(3, 2, 90, true, "Remix");
    pad(3, 3, 95, true, "Echo");
    pad(3, 4, 111, true, "Glitch");
    pad(3, 5, 108, false, "Load Preset");
    return f.state();
}
struct Factory { const char *name; std::string state; };
static std::vector<Factory> &factory() {
    static std::vector<Factory> F;
    if (F.empty()) {
        F.push_back({"LC > EUCLIDIER FX", factory_euclidier()});
        F.push_back({"LC 4 PAGES > CH 1-4", factory_pages(false)});
        F.push_back({"LC 4 PAGES > OUT A-D", factory_pages(true)});
        F.push_back({"EMPTY (LEARN)", "pg=0;"});
        F.push_back({"LC > DRUMGEN (CH 15)", factory_drumgen()});
    }
    return F;
}

/* Next to the plugin's own folder rather than inside it, so reinstalling the plugin folder
 * doesn't take the presets with it; inside it if the parent can't be written. /proc/self/maps
 * has the path. CCMAP_PRESETS overrides the place (offline test). */
static std::string so_dir() {
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/ccmap.so");
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    g_bank_path.clear();
    if (const char *env = std::getenv("CCMAP_PRESETS")) g_bank_path = env;
    else {
        std::string dir = so_dir(), cand[2];
        if (dir.empty()) dir = "/tmp";
        size_t cut = dir.rfind('/');
        cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/ccmap_presets.txt";
        cand[1] = dir + "/ccmap_presets.txt";
        for (const std::string &c : cand)                     /* an existing file wins ... */
            if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
        for (int i = 0; i < 2 && g_bank_path.empty(); i++)    /* ... else the first place we may write */
            if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    }
    if (g_bank_path.empty()) return;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        static char line[32768];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NSLOTS) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
        }
        std::fclose(f);
    }
    LOG("[ccmap_vst] preset bank %s\n", g_bank_path.c_str());
}
static bool bank_write_locked() {   /* whole file, via a temp file so a power cut can't leave half a bank */
    if (g_bank_path.empty()) return false;
    std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (int i = 0; i < NSLOTS; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
}
static std::string slot_title(int i) {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    char buf[32];
    if (g_slot_chunk[i].empty()) {
        if (i < (int)factory().size()) return factory()[i].name;
        std::snprintf(buf, sizeof buf, "%02d (empty)", i + 1);
        return buf;
    }
    if (!g_slot_name[i].empty()) return g_slot_name[i];
    std::snprintf(buf, sizeof buf, "CC Map %02d", i + 1);
    return buf;
}
static void slot_load(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    std::string c;
    {
        std::lock_guard<std::mutex> lk(g_bank_lock);
        c = g_slot_chunk[n];
        if (c.empty() && n < (int)factory().size()) c = factory()[n].state;
    }
    if (c.empty()) return;              /* an empty preset leaves the current map alone */
    std::lock_guard<std::mutex> lk(w->mu);
    w->mp.apply(c);
    collect(w);
    mark_all(w);
}
static bool slot_save(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    std::string c;
    { std::lock_guard<std::mutex> lk(w->mu); c = w->mp.state(); }
    std::lock_guard<std::mutex> lk(g_bank_lock);
    g_slot_chunk[n] = c;
    return bank_write_locked();
}

/* ---- ALSA: one thread does all of it ---------------------------------------- */
#ifndef NO_ALSA
static std::string lower(std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }
/* which device a client is, by its name; -1 = never listen to it */
static int classify(Plugin *w, int client) {
    auto it = w->dev_of.find(client);
    if (it != w->dev_of.end()) return it->second;
    int dev = -1;
    snd_seq_client_info_t *ci;
    snd_seq_client_info_alloca(&ci);
    if (client != snd_seq_client_id(w->seq) && client != SND_SEQ_CLIENT_SYSTEM &&
        snd_seq_get_any_client_info(w->seq, client, ci) >= 0) {
        const std::string n = lower(snd_seq_client_info_get_name(ci));
        if (n.find("launch control xl") != std::string::npos) dev = DEV_USB;   /* other LEDs, other layout */
        else if (n.find("launch control") != std::string::npos || n.find("launchcontrol") != std::string::npos) dev = DEV_LC;
        else if (n.find("uc-33") != std::string::npos || n.find("uc33") != std::string::npos) dev = DEV_UC;
        else if (n.find("ec4") != std::string::npos) dev = DEV_EC4;
        else if (n.find("e16") != std::string::npos) dev = DEV_E16;   /* "OXI E16" (OXI One / Coral stay USB) */
        else if (n.find("force") != std::string::npos || n.find("mpc") != std::string::npos ||
                 n.find("through") != std::string::npos) dev = -1;           /* the device itself */
        else dev = DEV_USB;
        LOG("[ccmap_vst] client %d '%s' -> %s\n", client, snd_seq_client_info_get_name(ci),
            dev == DEV_LC ? "Launch Control" : dev == DEV_UC ? "UC-33" : dev == DEV_EC4 ? "Faderfox EC4" : dev == DEV_E16 ? "OXI E16" :
            dev == DEV_USB ? "USB controller" : "ignored");
    }
    w->dev_of[client] = dev;
    return dev;
}
/* listen to one hardware MIDI input (a USB controller, the DIN input) */
static void subscribe(Plugin *w, int client, int port) {
    if (client == snd_seq_client_id(w->seq) || client == SND_SEQ_CLIENT_SYSTEM) return;
    snd_seq_port_info_t *pi;
    snd_seq_port_info_alloca(&pi);
    if (snd_seq_get_any_port_info(w->seq, client, port, pi) < 0) return;
    const unsigned cap = snd_seq_port_info_get_capability(pi), type = snd_seq_port_info_get_type(pi);
    if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) return;
    if (!(type & SND_SEQ_PORT_TYPE_HARDWARE)) return;         /* not other plugins' virtual ports */
    const int dev = classify(w, client);
    if (dev < 0) return;
    /* a controller that can take values back (FEEDBACK): remember where, and show it the map */
    /* the first writable port only: the E16's further USB ports may lead to its TRS outputs */
    if ((has_display(dev) || dev == DEV_USB) && (cap & SND_SEQ_PORT_CAP_WRITE) && !w->fb_port.count(client)) {
        w->fb_port[client] = port;
        LOG("[ccmap_vst] feedback -> %d:%d '%s'\n", client, port, snd_seq_port_info_get_name(pi));
        std::lock_guard<std::mutex> lk(w->mu);
        w->mp.feedback_all(dev);
    }
    if ((cap & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) return;
    int rc = snd_seq_connect_from(w->seq, w->in_port, client, port);
    LOG("[ccmap_vst] in <- %d:%d '%s'%s\n", client, port, snd_seq_port_info_get_name(pi),
        rc < 0 ? " (already connected or refused)" : "");
}
/* Link A: Out A -> every other client's port named "Control In" */
static void link_scan(Plugin *w, bool on) {
    if (!on) {
        for (auto &a : w->linked) snd_seq_disconnect_to(w->seq, w->out_port[0], a.first, a.second);
        if (!w->linked.empty()) LOG("[ccmap_vst] link A off (%d ports)\n", (int)w->linked.size());
        w->linked.clear();
        return;
    }
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(w->seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        if (c == snd_seq_client_id(w->seq)) continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(w->seq, pi) >= 0) {
            if (std::strcmp(snd_seq_port_info_get_name(pi), "Control In")) continue;
            const int p = snd_seq_port_info_get_port(pi);
            if (std::find(w->linked.begin(), w->linked.end(), std::make_pair(c, p)) != w->linked.end()) continue;
            if (snd_seq_connect_to(w->seq, w->out_port[0], c, p) >= 0) {
                w->linked.push_back({c, p});
                LOG("[ccmap_vst] link A -> %d:%d '%s Control In'\n", c, p, snd_seq_client_info_get_name(ci));
            }
        }
    }
}
static void scan_inputs(Plugin *w) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(w->seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(w->seq, pi) >= 0) subscribe(w, c, snd_seq_port_info_get_port(pi));
    }
}
static void send_msg(Plugin *w, const Msg &m) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    const bool fb = m.port >= FB_PORT;   /* FEEDBACK: back to every controller of that kind */
    if (fb) snd_seq_ev_set_source(&ev, w->led_port);
    else { snd_seq_ev_set_source(&ev, w->out_port[m.port]); snd_seq_ev_set_subs(&ev); }
    snd_seq_ev_set_direct(&ev);
    const int t = m.d[0] & 0xF0, ch = m.d[0] & 0x0F;
    switch (t) {
    case 0x90: snd_seq_ev_set_noteon(&ev, ch, m.d[1], m.d[2]); break;
    case 0x80: snd_seq_ev_set_noteoff(&ev, ch, m.d[1], m.d[2]); break;
    case 0xB0: snd_seq_ev_set_controller(&ev, ch, m.d[1], m.d[2]); break;
    case 0xC0: snd_seq_ev_set_pgmchange(&ev, ch, m.d[1]); break;
    case 0xD0: snd_seq_ev_set_chanpress(&ev, ch, m.d[1]); break;
    case 0xA0: snd_seq_ev_set_keypress(&ev, ch, m.d[1], m.d[2]); break;
    case 0xE0: snd_seq_ev_set_pitchbend(&ev, ch, ((m.d[2] << 7) | m.d[1]) - 8192); break;
    default: return;
    }
    if (!fb) { snd_seq_event_output_direct(w->seq, &ev); w->n_out++; return; }
    for (auto &a : w->fb_port) {
        auto d = w->dev_of.find(a.first);
        if (d == w->dev_of.end() || d->second != m.port - FB_PORT) continue;
        snd_seq_ev_set_dest(&ev, a.first, a.second);
        snd_seq_event_output_direct(w->seq, &ev);
        w->n_fb++;
    }
}
static void send_lc_sysex(Plugin *w, const uint8_t *d, int n) {
    if (w->lc_client < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->led_port);
    snd_seq_ev_set_dest(&ev, w->lc_client, w->lc_port);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_sysex(&ev, n, (void *)d);
    snd_seq_event_output_direct(w->seq, &ev);
}
/* LEDs: SysEx F0 00 20 29 02 0A 78 <template> <index> <value> F7, only what changed */
static void leds_update(Plugin *w, const uint8_t v[NLEDS], int tmpl) {
    if (w->lc_client < 0) return;
    if (tmpl != w->led_tmpl) { std::memset(w->led_sent, 0xFF, sizeof w->led_sent); w->led_tmpl = tmpl; }
    for (int i = 0; i < NLEDS; i++) {
        if (w->led_sent[i] == v[i]) continue;
        const uint8_t sx[11] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0A, 0x78, (uint8_t)tmpl, (uint8_t)i, v[i], 0xF7};
        send_lc_sysex(w, sx, sizeof sx);
        w->led_sent[i] = v[i];
    }
}
static void kit_return(Plugin *w, int ch, int cc, int val);
static void handle(Plugin *w, snd_seq_event_t *ev) {
    if (w->ret_port >= 0 && ev->dest.client == snd_seq_client_id(w->seq) && ev->dest.port == w->ret_port) {
        if (ev->type == SND_SEQ_EVENT_CONTROLLER) kit_return(w, ev->data.control.channel, (int)ev->data.control.param, ev->data.control.value);
        return;
    }
    switch (ev->type) {
    case SND_SEQ_EVENT_PORT_START:
        subscribe(w, ev->data.addr.client, ev->data.addr.port);
        if (w->link_applied) link_scan(w, true);
        return;
    case SND_SEQ_EVENT_CLIENT_START:
    case SND_SEQ_EVENT_CLIENT_EXIT:
        w->dev_of.erase(ev->data.addr.client);
        if (ev->type == SND_SEQ_EVENT_CLIENT_EXIT) {
            if (ev->data.addr.client == w->lc_client) w->lc_client = -1;
            w->fb_port.erase(ev->data.addr.client);
            w->linked.erase(std::remove_if(w->linked.begin(), w->linked.end(),
                            [&](const std::pair<int, int> &a) { return a.first == ev->data.addr.client; }), w->linked.end());
        }
        return;
    default: break;
    }
    const int dev = classify(w, ev->source.client);
    if (dev < 0) { w->n_ignored++; return; }
    if (dev == DEV_LC && (w->lc_client != ev->source.client || w->lc_port != ev->source.port)) {
        w->lc_client = ev->source.client; w->lc_port = ev->source.port; w->led_tmpl = -1;
    }
    int st = -1, d1 = 0, d2 = 0;
    const snd_seq_ev_note_t &n = ev->data.note;
    const snd_seq_ev_ctrl_t &c = ev->data.control;
    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON: st = 0x90 | n.channel; d1 = n.note; d2 = n.velocity; break;
    case SND_SEQ_EVENT_NOTEOFF: st = 0x80 | n.channel; d1 = n.note; d2 = n.velocity; break;
    case SND_SEQ_EVENT_CONTROLLER: st = 0xB0 | c.channel; d1 = (int)c.param; d2 = clampi(c.value, 0, 127); break;
    case SND_SEQ_EVENT_PGMCHANGE: st = 0xC0 | c.channel; d1 = clampi(c.value, 0, 127); break;
    case SND_SEQ_EVENT_CHANPRESS: st = 0xD0 | c.channel; d1 = clampi(c.value, 0, 127); break;
    case SND_SEQ_EVENT_KEYPRESS: st = 0xA0 | n.channel; d1 = n.note; d2 = n.velocity; break;
    case SND_SEQ_EVENT_PITCHBEND: { const int v = clampi(c.value + 8192, 0, 16383); st = 0xE0 | c.channel; d1 = v & 0x7f; d2 = v >> 7; break; }
    case SND_SEQ_EVENT_SYSEX:
        if (dev == DEV_LC) {
            std::lock_guard<std::mutex> lk(w->mu);
            w->mp.lc_sysex((const uint8_t *)ev->data.ext.ptr, (int)ev->data.ext.len);
            collect(w);
        }
        return;
    default: return;
    }
    std::lock_guard<std::mutex> lk(w->mu);
    if (w->mp.s.input == 0 && dev != DEV_LC && dev != DEV_UC && !has_display(dev)) { w->n_ignored++; return; }
    w->n_in++;
    w->mp.input(dev, st, d1, d2);
    collect(w);
}
static void thread_main(Plugin *w) {
    double t_log = 0;
    std::vector<Msg> out;
    while (w->running.load()) {
        int npfd = snd_seq_poll_descriptors_count(w->seq, POLLIN);
        std::vector<pollfd> pf((size_t)npfd + 1);
        snd_seq_poll_descriptors(w->seq, pf.data(), (unsigned)npfd, POLLIN);
        pf[(size_t)npfd].fd = w->wake[0]; pf[(size_t)npfd].events = POLLIN; pf[(size_t)npfd].revents = 0;
        poll(pf.data(), pf.size(), 50);
        if (pf[(size_t)npfd].revents & POLLIN) { char b[64]; while (read(w->wake[0], b, sizeof b) > 0) {} }
        snd_seq_event_t *ev = nullptr;
        for (int guard = 0; guard < 512 && snd_seq_event_input_pending(w->seq, 1) > 0; guard++) {
            if (snd_seq_event_input(w->seq, &ev) < 0 || !ev) break;
            handle(w, ev);
        }
        /* everything the mapper queued (from MIDI or from the host's parameter calls) */
        uint8_t v[NLEDS];
        int tmpl, link;
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double t_now = now.tv_sec + now.tv_nsec * 1e-9;
        {
            std::lock_guard<std::mutex> lk(w->mu);
            out.assign(w->mp.q, w->mp.q + w->mp.nq);
            w->mp.nq = 0;
            if (w->mp.flash_req) { w->mp.flash_req = false; w->flash_until = t_now + 1.2; }
            w->mp.leds(v, t_now < w->flash_until);
            tmpl = w->mp.lc_ch;
            link = w->mp.s.link;
        }
        for (const Msg &m : out) send_msg(w, m);
        leds_update(w, v, tmpl);
        if (link != w->link_applied) { w->link_applied = link; link_scan(w, link != 0); }
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double t = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (t - t_log > 5.0) {
            if (t_log > 0 && (w->n_in || w->n_out || w->n_fb || w->n_ignored))
                LOG("[ccmap_vst] 5 s: %ld in, %ld out, %ld feedback, %ld ignored, %d linked, LC %s (template %d)\n",
                    w->n_in.exchange(0), w->n_out.exchange(0), w->n_fb.exchange(0), w->n_ignored.exchange(0), (int)w->linked.size(),
                    w->lc_client >= 0 ? "found" : "not seen", tmpl + 1);
            t_log = t;
        }
    }
}
static void wake(Plugin *w) { if (w->wake[1] >= 0) { char b = 1; (void)!write(w->wake[1], &b, 1); } }
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        w->seq = nullptr; LOG("[ccmap_vst] snd_seq_open failed\n"); return;
    }
    snd_seq_set_client_pool_output(w->seq, 2048);
    snd_seq_set_client_name(w->seq, PLUG_NAME);
    /* hidden input: we subscribe it ourselves, it doesn't show up in the Force's MIDI lists */
    w->in_port = snd_seq_create_simple_port(w->seq, "In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT,
                                            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    w->led_port = snd_seq_create_simple_port(w->seq, "LED", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_NO_EXPORT,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    static const char *const NAMES[NOUTS] = {"Out A", "Out B", "Out C", "Out D"};
    for (int i = 0; i < NOUTS; i++)
        w->out_port[i] = snd_seq_create_simple_port(w->seq, NAMES[i], SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                                    SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    /* back channel: whoever sends here (the Force as a MIDI output, another plugin) moves the kit's elements */
    w->ret_port = snd_seq_create_simple_port(w->seq, "Return", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[ccmap_vst] ALSA client '" PLUG_NAME "' (%d), out ports %d..%d\n", snd_seq_client_id(w->seq), w->out_port[0], w->out_port[3]);
    if (w->in_port < 0 || w->out_port[0] < 0) { LOG("[ccmap_vst] port creation failed\n"); return; }
    /* hot-plug: the system announce port tells us about ports that appear later */
    snd_seq_connect_from(w->seq, w->in_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    scan_inputs(w);
    std::memset(w->led_sent, 0xFF, sizeof w->led_sent);
    if (pipe(w->wake) == 0) {
        for (int i = 0; i < 2; i++) { int fl = fcntl(w->wake[i], F_GETFL); fcntl(w->wake[i], F_SETFL, fl | O_NONBLOCK); }
    } else w->wake[0] = w->wake[1] = -1;
    w->running.store(true);
    w->thr = std::thread(thread_main, w);
}
static void alsa_close(Plugin *w) {
    if (w->running.exchange(false)) { wake(w); if (w->thr.joinable()) w->thr.join(); }
    if (!w->seq) return;
    /* held notes off, LEDs off (Bn 00 00 = reset on the template's channel) */
    {
        std::lock_guard<std::mutex> lk(w->mu);
        w->mp.release_all();
        for (int i = 0; i < w->mp.nq; i++) send_msg(w, w->mp.q[i]);
        w->mp.nq = 0;
        if (w->lc_client >= 0) {
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, w->led_port);
            snd_seq_ev_set_dest(&ev, w->lc_client, w->lc_port);
            snd_seq_ev_set_direct(&ev);
            snd_seq_ev_set_controller(&ev, w->mp.lc_ch, 0, 0);
            snd_seq_event_output_direct(w->seq, &ev);
        }
    }
    snd_seq_close(w->seq);
    w->seq = nullptr;
    for (int i = 0; i < 2; i++) if (w->wake[i] >= 0) close(w->wake[i]);
}
#else
static void wake(Plugin *) {}
static void alsa_open(Plugin *) {}
static void alsa_close(Plugin *) {}
/* offline test hooks (host_test.cpp): feed a controller message, take what would go out, read the LEDs */
extern "C" __attribute__((visibility("default"))) void ccmap_test_input(AEffect *e, int dev, int st, int d1, int d2) {
    Plugin *w = (Plugin *)e->object;
    std::lock_guard<std::mutex> lk(w->mu);
    w->mp.input(dev, st, d1, d2);
    collect(w);
}
extern "C" __attribute__((visibility("default"))) void ccmap_test_sysex(AEffect *e, const uint8_t *d, int n) {
    Plugin *w = (Plugin *)e->object;
    std::lock_guard<std::mutex> lk(w->mu);
    w->mp.lc_sysex(d, n);
    collect(w);
}
extern "C" __attribute__((visibility("default"))) int ccmap_test_take(AEffect *e, uint8_t *buf, int max) {
    Plugin *w = (Plugin *)e->object;
    std::lock_guard<std::mutex> lk(w->mu);
    int n = 0;
    for (int i = 0; i < w->mp.nq && n < max; i++, n++) {
        buf[4 * n] = w->mp.q[i].port;
        std::memcpy(buf + 4 * n + 1, w->mp.q[i].d, 3);
        if (w->mp.q[i].len == 2) buf[4 * n + 3] = 0;
    }
    w->mp.nq = 0;
    return n;
}
extern "C" __attribute__((visibility("default"))) void ccmap_test_leds(AEffect *e, uint8_t *v) {
    Plugin *w = (Plugin *)e->object;
    std::lock_guard<std::mutex> lk(w->mu);
    w->mp.leds(v);
}
/* the template-change flash: returns whether one was requested (and clears it), v = the flash picture */
extern "C" __attribute__((visibility("default"))) int ccmap_test_flash(AEffect *e, uint8_t *v) {
    Plugin *w = (Plugin *)e->object;
    std::lock_guard<std::mutex> lk(w->mu);
    const int r = w->mp.flash_req;
    w->mp.flash_req = false;
    w->mp.leds(v, true);
    return r;
}
#endif

/* ---- RETURN: a CC on "CC Map Return" moves the kit's elements (the page polls their positions) ---- */
static std::atomic<long> g_ret_count{0};
static void kit_return(Plugin *w, int ch, int cc, int val) {
    int n;
    { std::lock_guard<std::mutex> lk(w->mu); n = w->kit.feedback(ch, cc, val); }
    if (g_ret_count++ < 20) LOG("[ccmap_vst] return: ch %d cc %d = %d -> %d kit elements\n", ch + 1, cc, val, n);
}
#ifdef NO_ALSA
extern "C" __attribute__((visibility("default"))) void ccmap_test_return(AEffect *e, int ch, int cc, int val) {
    kit_return((Plugin *)e->object, ch, cc, val);
}
#endif

/* ---- phone view (ccmap_web.h) ---------------------------------------------------------- */
static std::string state_json(Plugin *w) {
    std::lock_guard<std::mutex> lk(w->mu);
    const Mapper &mp = w->mp;
    std::string j;
    char b[160], tgt[48], src[48], tn[32];
    uint8_t led[NLEDS];
    mp.leds(led);
    mp.template_name(tn, sizeof tn);
    std::snprintf(b, sizeof b, "{\"lc\":%s,\"tmpl\":%s,\"tmpl_led\":%d,\"page\":%d,\"learn\":%s,\"last_slot\":%d,\"last\":",
                  mp.lc_seen ? "true" : "false", ccweb::js(tn).c_str(), Mapper::template_colour(mp.lc_ch), mp.s.page + 1,
                  mp.learn ? "true" : "false", mp.last_slot);
    j = b;
    j += ccweb::js(mp.last);
    /* the Launch Control of the active template and page: 16 knobs, 8 pads */
    auto lc_slot = [&](int stype, int num) {
        for (int i = 0; i < NMAP; i++) if (mp.on_page(mp.map[i]) && mp.matches(mp.map[i], DEV_LC, mp.lc_ch, stype, num)) return i;
        return -1;
    };
    auto item = [&](int i, const std::string extra) {   /* by value: callers may pass b, which item() reuses */
        std::string o;
        if (i < 0) { o = std::string("{\"slot\":-1,\"label\":\"\",\"target\":\"\",\"val\":-1") + extra + "}"; return o; }
        const Map &m = mp.map[i];
        mp.target_text(m, tgt, sizeof tgt);
        int val = m.val;
        if (m.stype == S_NOTE) val = (m.btn == B_TOGGLE ? mp.run[i].on : mp.run[i].high) ? 127 : 0;
        std::snprintf(b, sizeof b, "{\"slot\":%d,\"val\":%d,\"label\":", i, val);
        o = b;
        o += ccweb::js(m.name);
        o += ",\"target\":" + ccweb::js(tgt) + extra + "}";
        return o;
    };
    j += ",\"knobs\":[";
    for (int k = 0; k < 16; k++) j += (k ? "," : "") + item(lc_slot(S_CC, k < 8 ? 21 + k : 41 + k - 8), "");
    j += "],\"pads\":[";
    for (int p = 0; p < 8; p++) {
        std::snprintf(b, sizeof b, ",\"led\":%d", led[p]);
        j += (p ? "," : "") + item(lc_slot(S_NOTE, p < 4 ? 9 + p : 25 + p - 4), std::string(b));
    }
    /* every other slot of the page (EC4, E16, UC-33, other USB, other LC templates are left out) */
    j += "],\"others\":[";
    bool first = true;
    for (int i = 0; i < NMAP; i++) {
        const Map &m = mp.map[i];
        if (!m.used || m.dev == DEV_LC || !mp.on_page(m)) continue;
        src_name(src, sizeof src, m.dev, m.sch, m.stype, m.snum);
        const std::string extra = ",\"src\":" + ccweb::js(src);
        j += (first ? "" : ",") + item(i, extra);
        first = false;
    }
    j += "]}";
    return j;
}
static void web_label(Plugin *w, int slot, const std::string &name) {
    { std::lock_guard<std::mutex> lk(w->mu); w->mp.set_name(slot, name); collect(w); }
    w->redisplay.store(true);
    LOG("[ccmap_vst] slot %d named '%s' (phone view)\n", slot + 1, name.c_str());
}
/* ---- BAUKASTEN (ccmap_kit.h): one library file for every project, next to the presets ---------- */
static std::mutex g_kit_file_lock;
static std::string kit_path() {
    if (const char *env = std::getenv("CCMAP_KIT")) return env;
    std::string bank;
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank = g_bank_path; }
    const size_t cut = bank.rfind('/');
    if (cut == std::string::npos) return "/tmp/ccmap_kit.txt";
    return bank.substr(0, cut) + "/ccmap_kit.txt";
}
static void kit_load(Plugin *w) {
    const std::string path = kit_path();
    std::string t;
    {
        std::lock_guard<std::mutex> lk(g_kit_file_lock);
        if (FILE *f = std::fopen(path.c_str(), "r")) {
            char b[4096];
            size_t n;
            while ((n = std::fread(b, 1, sizeof b, f)) > 0 && t.size() < (1u << 20)) t.append(b, n);
            std::fclose(f);
        }
    }
    std::lock_guard<std::mutex> lk(w->mu);
    if (!t.empty() && w->kit.from_text(t)) LOG("[ccmap_vst] kit %s: %d pages, %d elements\n", path.c_str(), (int)w->kit.pages.size(), w->kit.count());
    else LOG("[ccmap_vst] kit %s: none yet\n", path.c_str());
}
/* whole file via a temp file (a power cut can't leave half a kit); never with w->mu held */
static bool kit_write(const std::string &text) {
    const std::string path = kit_path(), tmp = path + ".tmp";
    std::lock_guard<std::mutex> lk(g_kit_file_lock);
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) { LOG("[ccmap_vst] kit: cannot write %s\n", tmp.c_str()); return false; }
    std::fwrite(text.data(), 1, text.size(), f);
    const bool ok = std::fclose(f) == 0 && std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) LOG("[ccmap_vst] kit: cannot save %s\n", path.c_str());
    return ok;
}
/*   GET /kit                              all pages with their elements (JSON)
 *   GET /vals?p=P                         positions of page P (polled while playing)
 *   GET /send?p=P&i=I&v=V[&v2=V2]         element I of page P to position V (XY: V2 = Y), 0..127
 *   GET /page?op=add|set|del|move&p=P&name=N&cols=C&dir=-1|1
 *   GET /el?op=put|del|move|dup&p=P&i=I (put with i=-1: new) &t=fader|knob|button|toggle|xy|picker|strip&n=Name
 *           &c=colour&o=0..3&ch=1..16&cc=&cc2=&lo=&hi=&w=&h=&st=step&lb=labels,comma,separated&dir=-1|1
 *   (/send for a PICKER: V is the value to send itself, not a position; for a STRIP: V = fader, V2 = mute, -1 = unchanged)
 *   GET /import                           a new page from the CC Map slots of the active page
 * Edits answer with the whole kit (like /kit) and are saved to the SD card at once. */
/* whether the query has KEY= as a whole key (forcehub's Request::has() also matches "cc=" for "c") */
static bool qhas(const forcehub::Request &r, const char *key) {
    const std::string k = std::string(key) + "=";
    for (size_t p = r.query.find(k); p != std::string::npos; p = r.query.find(k, p + 1))
        if (p == 0 || r.query[p - 1] == '&') return true;
    return false;
}
static int kit_route(Plugin *w, int c, const forcehub::Request &r) {
    using namespace forcehub;
    using cckit::El;
    using cckit::Page;
    const int p = r.num("p", -1), i = r.num("i", -1);
    if (r.route == "/kit") { std::lock_guard<std::mutex> lk(w->mu); ok_json(c, w->kit.json()); return DONE; }
    if (r.route == "/vals") { std::lock_guard<std::mutex> lk(w->mu); ok_json(c, w->kit.values(p)); return DONE; }
    if (r.route == "/send") {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(w->mu);
            cckit::Kit &k = w->kit;
            if (p >= 0 && p < (int)k.pages.size() && i >= 0 && i < (int)k.pages[p].el.size() && qhas(r, "v")) {
                El &e = k.pages[p].el[i];
                cckit::Kit::send(e, r.num("v"), r.num("v2", -1), [w](int port, int st, int d1, int d2) { w->mp.emit(port, st, d1, d2); });
                const std::string nm = e.name.empty() ? "KIT CC " + std::to_string(e.cc) : e.name;
                if (e.type == cckit::T_STRIP && r.num("v", -1) < 0)
                    std::snprintf(w->mp.last, sizeof w->mp.last, "%s MUTE %s", nm.c_str(), e.v2 >= 64 ? "ON" : "OFF");
                else if (e.type == cckit::T_XY) std::snprintf(w->mp.last, sizeof w->mp.last, "%s = %d / %d", nm.c_str(), e.last, e.last2);
                else if (e.type == cckit::T_PICKER && !cckit::Kit::label_for(e, e.last).empty())
                    std::snprintf(w->mp.last, sizeof w->mp.last, "%s = %s (%d)", nm.c_str(), cckit::Kit::label_for(e, e.last).c_str(), e.last);
                else std::snprintf(w->mp.last, sizeof w->mp.last, "%s = %d", nm.c_str(), e.last);
                w->mp.last_slot = -1;
                w->mp.changed |= CH_LAST;
                collect(w);
                ok = true;
            }
        }
        if (!ok) { bad(c, "no such element"); return DONE; }
        wake(w);   /* the ALSA thread sends what was queued */
        ok_json(c, "{\"ok\":true}");
        return DONE;
    }
    if (r.route != "/page" && r.route != "/el" && r.route != "/import") return NOT_MINE;
    const std::string op = r.arg("op");
    const char *err = nullptr;
    std::string text, json;
    {
        std::lock_guard<std::mutex> lk(w->mu);
        cckit::Kit &k = w->kit;
        const int np = (int)k.pages.size();
        const bool page_ok = p >= 0 && p < np;
        if (r.route == "/import") {
            int skipped = 0;
            Page pg = cckit::Kit::from_mapper(w->mp, skipped);
            if (np >= cckit::MAXPAGES) err = "kit full (16 pages)";
            else if (pg.el.empty()) err = "no CC slots on the active page";
            else { k.pages.push_back(pg); LOG("[ccmap_vst] kit: page imported from the map (%d elements, %d skipped)\n", (int)pg.el.size(), skipped); }
        } else if (r.route == "/page") {
            if (op == "add") {
                if (np >= cckit::MAXPAGES) err = "kit full (16 pages)";
                else {
                    Page pg;
                    pg.name = qhas(r, "name") ? r.arg("name") : "Seite " + std::to_string(np + 1);
                    if (qhas(r, "cols")) pg.cols = r.num("cols");
                    k.pages.push_back(pg);
                }
            } else if (!page_ok) err = "no such page";
            else if (op == "set") {
                if (qhas(r, "name")) k.pages[p].name = r.arg("name");
                if (qhas(r, "cols")) k.pages[p].cols = r.num("cols");
            } else if (op == "del") k.pages.erase(k.pages.begin() + p);
            else if (op == "move") {
                const int q = p + (r.num("dir") < 0 ? -1 : 1);
                if (q >= 0 && q < np) std::swap(k.pages[p], k.pages[q]);
            } else err = "op: add, set, del, move";
        } else {   /* /el */
            if (!page_ok) err = "no such page";
            else {
                std::vector<El> &el = k.pages[p].el;
                const bool el_ok = i >= 0 && i < (int)el.size();
                if (op == "put") {
                    if (!el_ok && i != -1) err = "no such element";
                    else if (!el_ok && (int)el.size() >= cckit::MAXEL) err = "page full (48 elements)";
                    else {
                        El e = el_ok ? el[i] : El();
                        if (qhas(r, "t")) { const std::string t = r.arg("t"); for (int x = 0; x < cckit::NTYPES; x++) if (t == cckit::type_key(x)) e.type = x; }
                        if (qhas(r, "n")) e.name = r.arg("n");
                        if (qhas(r, "c")) e.color = r.num("c");
                        if (qhas(r, "o")) e.out = r.num("o");
                        if (qhas(r, "ch")) e.ch = r.num("ch") - 1;
                        if (qhas(r, "cc")) e.cc = r.num("cc");
                        if (qhas(r, "cc2")) e.cc2 = r.num("cc2");
                        if (qhas(r, "lo")) e.lo = r.num("lo");
                        if (qhas(r, "hi")) e.hi = r.num("hi");
                        if (qhas(r, "w")) e.w = r.num("w");
                        if (qhas(r, "h")) e.h = r.num("h");
                        if (qhas(r, "st")) e.step = r.num("st");
                        if (qhas(r, "lb")) e.labels = r.arg("lb");
                        k.sanitize(e);
                        e.last = e.last2 = -1;   /* a changed target gets the next move in any case */
                        if (el_ok) el[i] = e; else el.push_back(e);
                    }
                } else if (!el_ok) err = "no such element";
                else if (op == "del") el.erase(el.begin() + i);
                else if (op == "dup") {
                    if ((int)el.size() >= cckit::MAXEL) err = "page full (48 elements)";
                    else { El e = el[i]; e.last = e.last2 = -1; el.insert(el.begin() + i + 1, e); }
                } else if (op == "move") {
                    const int q = i + (r.num("dir") < 0 ? -1 : 1);
                    if (q >= 0 && q < (int)el.size()) std::swap(el[i], el[q]);
                } else err = "op: put, del, move, dup";
            }
        }
        if (!err) {
            for (Page &pg : k.pages) k.sanitize(pg);
            k.rev++;
            text = k.to_text();
            json = k.json();
        }
    }
    if (err) { bad(c, err); return DONE; }
    kit_write(text);
    ok_json(c, json);
    return DONE;
}
static void kit_web_stop(Plugin *w) {
    if (w->kitweb) { w->kitweb->stop(); delete w->kitweb; w->kitweb = nullptr; }
}
/* after CC Map's own server, so CC Map keeps the first port */
static void kit_web_start(Plugin *w) {
    if (w->kitweb) return;
    w->kitweb = new forcehub::Server();
    w->kitweb->id = "ccmap-kit";
    w->kitweb->name = "Baukasten";
    w->kitweb->color = "#4fc3a1";
    w->kitweb->app = CCMAP_KIT_PAGE;
    w->kitweb->summary = [w] {
        std::lock_guard<std::mutex> lk(w->mu);
        const int np = (int)w->kit.pages.size(), ne = w->kit.count();
        if (!np) return std::string("Eigene Regler bauen");
        return std::to_string(np) + (np == 1 ? " Seite · " : " Seiten · ") + std::to_string(ne) + (ne == 1 ? " Element" : " Elemente");
    };
    w->kitweb->route = [w](int c, const forcehub::Request &r) { return kit_route(w, c, r); };
    if (!w->kitweb->start()) { LOG("[ccmap_vst] kit: no free port\n"); delete w->kitweb; w->kitweb = nullptr; }
    else LOG("[ccmap_vst] kit on http://%s\n", w->kitweb->address().c_str());
}

/* start or stop the phone view to match the WEB VIEW setting; never call with w->mu held */
static void web_apply(Plugin *w) {
    if (!w->owner) return;
    bool want;
    { std::lock_guard<std::mutex> lk(w->mu); want = w->mp.s.web != 0; }
    if (want && !w->web) {
        w->hooks.state = [w] { return state_json(w); };
        w->hooks.set_label = [w](int i, const std::string &n) { web_label(w, i, n); };
        w->hooks.remote = [w](int i, int v) {
            bool ok;
            { std::lock_guard<std::mutex> lk(w->mu); ok = w->mp.remote_set(i, v); collect(w); }
            wake(w);   /* the ALSA thread sends what the slot queued */
            return ok;
        };
        w->hooks.files_on = [w] { std::lock_guard<std::mutex> lk(w->mu); return w->mp.s.files != 0; };
        w->web = new forcehub::Server();
        w->web->id = "ccmap";
        w->web->name = "CC Map";
        w->web->color = "#ff8a2a";
        w->web->app = CCMAP_PAGE;
        w->web->summary = [w] {
            std::lock_guard<std::mutex> lk(w->mu);
            char tn[32], b[96];
            w->mp.template_name(tn, sizeof tn);
            std::snprintf(b, sizeof b, "%s · Page %d%s", w->mp.lc_seen ? tn : "kein Launch Control", w->mp.s.page + 1,
                          w->mp.learn ? " · LEARN" : "");
            return std::string(b);
        };
        forcehub::Server *sv = w->web;
        w->web->route = [w, sv](int c, const forcehub::Request &r) { return ccweb::route(*sv, w->hooks, c, r); };
        if (!w->web->start()) { LOG("[ccmap_vst] phone view: no free port %d..%d\n", forcehub::Server::base_port(), forcehub::Server::base_port() + 9); delete w->web; w->web = nullptr; }
        else LOG("[ccmap_vst] phone view on http://%s (hub HUB_VERSION %d)\n", w->web->address().c_str(), HUB_VERSION);
        if (w->web) kit_web_start(w);
    } else if (!want && w->web) {
        kit_web_stop(w);
        w->web->stop(); delete w->web; w->web = nullptr;
        LOG("[ccmap_vst] phone view off\n");
    }
    w->redisplay.store(true);
}

/* ---- audio: pass through, report our own parameter changes to the host ---------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    for (int ch = 0; ch < 2; ch++) {   /* the host may hand us in == out */
        if (in && in[ch]) { if (out[ch] != in[ch]) std::memmove(out[ch], in[ch], (size_t)n * sizeof(float)); }
        else std::memset(out[ch], 0, (size_t)n * sizeof(float));
    }
    bool any = false;
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    uint64_t bits = w->pending.load();
    if (bits && w->mu.try_lock()) {   /* never wait in the audio thread: try again next block */
        bits = w->pending.exchange(0);
        float val[NPARAMS];
        for (int i = 0; i < NPARAMS; i++)
            if (bits & (1ull << i)) val[i] = KEY_OF[i] >= 0 ? ui_to_norm(&PARAMS[i], ui_value(w, KEY_OF[i])) : 0.0f;
        w->mu.unlock();
        for (int i = 0; i < NPARAMS; i++)
            if (bits & (1ull << i)) { any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, val[i]); }
    }
    if (w->redisplay.exchange(false)) any = true;
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

/* ---- parameters ------------------------------------------------------------------ */
/* the editor changes a target: a note still held on the old target is released first */
static void edit_slot(Plugin *w) {
    Mapper &mp = w->mp;
    Run &r = mp.run[mp.sel];
    const Map &m = mp.map[mp.sel];
    if (r.note >= 0) mp.emit(m.out, 0x80 | m.ch, r.note, 0);
    r = Run();
}
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key < 0 || key == SOURCE || key == MONITOR || key == TEMPLATE || key == WEB_ADDR) return;   /* readouts */
    if (key == PRESET) {   /* browsing only: LOAD loads, so turning the knob can't wipe the map */
        const int v = (int)std::lround(clamp01(n) * (NSLOTS - 1));
        if (v != w->cur.load()) { w->cur.store(v); w->redisplay.store(true); }
        return;
    }
    if (is_trigger(key)) {
        const bool down = n > 0.5f;
        const bool rising = down && !w->down[i];   /* an echo of our own automate must not fire again */
        w->down[i] = down;
        if (!rising) return;
        if (key == LOAD) { slot_load(w); web_apply(w); }
        else if (key == SAVE) slot_save(w);
        else if (key == SEND) {    /* FEEDBACK now: the controllers show the page's positions */
            std::lock_guard<std::mutex> lk(w->mu);
            w->mp.feedback_all();
        } else if (key == LEARN) {   /* arms LEARN; pressed again while armed: cancel */
            std::lock_guard<std::mutex> lk(w->mu);
            w->mp.learn = !w->mp.learn;
            w->mp.changed |= CH_LEARN;
            collect(w);
        } else {
            std::lock_guard<std::mutex> lk(w->mu);
            edit_slot(w);
            w->mp.clear_slot(w->mp.sel);
            w->mp.changed |= CH_SLOT;
            collect(w);
        }
        w->release[i] = 1;
        w->redisplay.store(true);
        wake(w);
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {   /* a Q-Link nudge lands between options -> step one option */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            std::lock_guard<std::mutex> lk(w->mu);
            const int cur = ui_value(w, key);
            n = (float)clampi(cur + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    const int u = norm_to_ui(p, n);
    {
        std::lock_guard<std::mutex> lk(w->mu);
        Mapper &mp = w->mp;
        Map &m = mp.map[mp.sel];
        switch (key) {
        case SLOT: mp.select(u - 1); break;
        case PAGE: m.page = (uint8_t)u; break;
        case OUT: if (m.out != u) { edit_slot(w); m.out = (uint8_t)u; } break;
        case CH: if (m.ch != u - 1) { edit_slot(w); m.ch = (uint8_t)(u - 1); } break;
        case TYPE: if (m.type != u) { edit_slot(w); m.type = (uint8_t)u; } break;
        case NUM: if (m.num != u) { edit_slot(w); m.num = (uint8_t)u; } break;
        case LO: if (m.lo != u) { m.lo = (uint8_t)u; mp.run[mp.sel].last = -1; } break;
        case HI: if (m.hi != u) { m.hi = (uint8_t)u; mp.run[mp.sel].last = -1; } break;
        case BTN: if (m.btn != u) { edit_slot(w); m.btn = (uint8_t)u; } break;
        case CUR_PAGE: mp.set_page(u); break;
        case FOLLOW: mp.s.follow = u; break;
        case LEDS: mp.s.leds = u; break;
        case THRU: mp.s.thru = u; break;
        case LINK: mp.s.link = u; break;
        case INPUT: mp.s.input = u; break;
        case ARROWS: mp.s.arrows = u; break;
        case ENC: if (m.enc != u) { edit_slot(w); m.enc = (uint8_t)u; } break;
        case FEEDBACK: if (mp.s.feedback != u) { mp.s.feedback = u; mp.feedback_all(); } break;
        case TAKEOVER: if (mp.s.takeover != u) { mp.s.takeover = u; mp.uncatch(); } break;
        case WEB: mp.s.web = u; break;
        case FILES: mp.s.files = u; LOG("[ccmap_vst] stems download %s\n", u ? "ON" : "off"); break;
        default: break;
        }
        collect(w);
        if (key == SLOT) w->pending.fetch_and(~(1ull << i));   /* the host knows this one */
    }
    if (key == WEB) web_apply(w);
    w->redisplay.store(true);
    wake(w);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    const int key = KEY_OF[i];
    if (key < 0 || is_trigger(key) || key == SOURCE || key == MONITOR || key == TEMPLATE || key == WEB_ADDR) return 0.0f;
    if (key == PRESET) return (float)w->cur.load() / (NSLOTS - 1);
    std::lock_guard<std::mutex> lk(w->mu);
    return ui_to_norm(&PARAMS[i], ui_value(w, key));
}

static void note_name(char *b, size_t n, int note) {
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(b, n, "%d  %s%d", note, N[note % 12], note / 12 - 2);   /* 60 = C3, as on the MPC */
}
static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    buf[0] = 0;
    if (popup_is(idx)) { std::snprintf(buf, n, "%s", pp->opts[norm_to_ui(pp, w->open[idx])]); return; }
    if (key < 0 || is_trigger(key)) return;
    if (key == PRESET) { std::snprintf(buf, n, "%s", slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str()); return; }
    if (key == WEB_ADDR) { std::snprintf(buf, n, "%s", w->web ? w->web->address().c_str() : "OFF"); return; }
    std::lock_guard<std::mutex> lk(w->mu);
    const Mapper &mp = w->mp;
    const Map &m = mp.map[mp.sel];
    char src[48];
    src_name(src, sizeof src, m.dev, m.sch, m.stype, m.snum);
    switch (key) {
    case SLOT: std::snprintf(buf, n, "%d %s", mp.sel + 1, m.name[0] ? m.name : m.used ? src : "(empty)"); return;
    case SOURCE:
        if (mp.learn) std::snprintf(buf, n, "MOVE A CONTROL ...");
        else if (m.used) std::snprintf(buf, n, "%s", src);
        else std::snprintf(buf, n, "- (LEARN)");
        return;
    case MONITOR: std::snprintf(buf, n, "%s", mp.last); return;
    case TEMPLATE: mp.template_name(buf, n); return;
    case NUM:
        if (m.type == D_NOTE) note_name(buf, n, m.num);
        else if (m.type == D_PROG) std::snprintf(buf, n, "- (MIN..MAX)");
        else if (m.type == D_PAGE) { if (m.num == 0) std::snprintf(buf, n, "NEXT PAGE"); else std::snprintf(buf, n, "PAGE %d", clampi(m.num, 1, 4)); }
        else std::snprintf(buf, n, "CC %d", m.num);
        return;
    case LO: case HI:
        if (m.type == D_PAGE) std::snprintf(buf, n, "-");
        else std::snprintf(buf, n, "%d", key == LO ? m.lo : m.hi);
        return;
    default: break;
    }
    const int u = ui_value(w, key);
    if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[clampi(u, 0, pp->nopts - 1)]);
    else std::snprintf(buf, n, "%d", u);
}

/* project chunk: "CCMP1;" + the mapper's state + "prog=<preset>;sel=<slot>;" */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "CCMP1;";
    {
        std::lock_guard<std::mutex> lk(w->mu);
        t += w->mp.state();
        char buf[48];
        std::snprintf(buf, sizeof buf, "prog=%d;sel=%d;", w->cur.load(), w->mp.sel);
        t += buf;
    }
    w->chunk.assign(t.begin(), t.end());
    w->chunk.push_back(0);
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 6) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));   /* stop at a NUL the host may have included */
    if (t.compare(0, 6, "CCMP1;")) return 0;
    t = t.substr(6);
    {
        std::lock_guard<std::mutex> lk(w->mu);
        w->mp.apply(t);
        size_t a = t.find("prog="), b = t.find("sel=");
        if (a != std::string::npos) w->cur.store(clampi(std::atoi(t.c_str() + a + 5), 0, NSLOTS - 1));
        if (b != std::string::npos) w->mp.sel = clampi(std::atoi(t.c_str() + b + 4), 0, NMAP - 1);
        collect(w);
        mark_all(w);
    }
    wake(w);
    web_apply(w);
    return 1;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: {
        kit_web_stop(w);
        if (w->web) { w->web->stop(); delete w->web; w->web = nullptr; }
        alsa_close(w);
        Plugin *me = w;
        g_owner.compare_exchange_strong(me, nullptr);
        LOG("[ccmap_vst] closed (owner=%d)\n", (int)w->owner);
        delete w;
        return 1;
    }
    case effSetProgram:
        if (v >= 0 && v < NSLOTS) { w->cur.store((int)v); slot_load(w); w->redisplay.store(true); wake(w); web_apply(w); }
        return 0;
    case effGetProgram: return w->cur.load();
    case effGetProgramName: if (p) copy_str(p, slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str(), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS || !p) return 0;
        copy_str(p, slot_title(idx).c_str(), 24);
        return 1;
    case effSetProgramName: {   /* only a saved preset has a line in the file to carry the name */
        const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || g_slot_chunk[n].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g_slot_name[n] = nm.substr(0, 23);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && KEY_OF[idx] >= 0 && !is_trigger(KEY_OF[idx]);
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[96];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: return 1;    /* the track's own MIDI is not used: the controllers come in over ALSA */
    case effCanDo:
        if (p && (!std::strcmp((const char *)p, "receiveVstEvents") || !std::strcmp((const char *)p, "receiveVstMidiEvent"))) return 1;
        return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!g_log) g_log = std::fopen("/tmp/ccmap_vst.log", "a");
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank_load_locked(); }
    Plugin *w = new Plugin();
    w->master = master;
    w->mp.reset();
    w->mp.changed = 0;
    Plugin *none = nullptr;
    w->owner = g_owner.compare_exchange_strong(none, w);
    if (w->owner) { kit_load(w); alsa_open(w); web_apply(w); }
    else LOG("[ccmap_vst] another CC Map instance already runs: this one stays passive\n");

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NSLOTS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[ccmap_vst] " PLUG_NAME " up (" BUILD_ID "), %d params, owner=%d\n", NPARAMS, (int)w->owner);
    return e;
}
