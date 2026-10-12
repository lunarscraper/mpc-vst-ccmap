/* =============================================================================
 * ccmap_core.h - the mapping engine of CC Map, free of VST and ALSA (so it can be tested offline).
 *
 * In:  channel messages from a hardware controller, tagged with the device they came from
 *      (Novation Launch Control, Evolution UC-33, Faderfox EC4, any other USB controller), plus
 *      the Launch Control's template-change SysEx.
 * Out: MIDI messages for one of the 4 output ports (Out A..D), FEEDBACK back to a controller that
 *      can show values (port FB_PORT + device, see feedback_all()), and the LED colours the
 *      Launch Control's 8 pads and 4 arrow buttons should show.
 *
 * NMAP mapping slots. A slot has a SOURCE (device, channel, CC or note number; filled by LEARN)
 * and a TARGET (output port, channel, type CC / NOTE / PROGRAM / PAGE, number, MIN..MAX, button
 * mode MOMENT or TOGGLE), and belongs to one of 4 PAGES or to ALL of them. A control may drive
 * several slots at once (one knob, several targets). MIN > MAX inverts.
 *
 * Every CC slot remembers where its control stands (Map::val, saved with the map), so that
 *  - FEEDBACK: on a page change, a preset or project load and when the controller turns up, the
 *    slots of the page send their positions back to a controller with value display / LED rings
 *    (Faderfox EC4): its encoders show the right value and the next turn doesn't jump;
 *  - PICKUP: an absolute pot without feedback (Launch Control, UC-33) only takes over a slot once
 *    it has reached the slot's value (or crossed it), instead of jumping;
 *  - ENCODER REL 64 / REL 2C: a relative encoder adds its steps to that value.
 * ========================================================================== */
#ifndef CCMAP_CORE_H
#define CCMAP_CORE_H
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ccmap {

enum Dev { DEV_ANY = 0, DEV_LC = 1, DEV_UC = 2, DEV_USB = 3, DEV_EC4 = 4, NDEV = 5 };
enum SrcType { S_CC = 0, S_NOTE = 1 };
enum DstType { D_OFF = 0, D_CC = 1, D_NOTE = 2, D_PROG = 3, D_PAGE = 4 };
enum BtnMode { B_MOMENT = 0, B_TOGGLE = 1 };
/* how a CC source counts: absolute, relative with 64 = no change (65 = +1, 63 = -1), relative
 * two's complement (1 = +1, 127 = -1) */
enum EncMode { ENC_ABS = 0, ENC_REL64 = 1, ENC_REL2C = 2 };
enum { FB_PORT = 16 };   /* Msg::port FB_PORT + Dev: back to that controller */
enum { PICKUP_NEAR = 3 };
enum { NMAP = 128, NPAGES = 4, NOUTS = 4, NLEDS = 12, QMAX = 256 };
/* Launch Control MK1 defaults (user and factory templates): knobs CC 21-28 / 41-48, pads notes
 * 9-12 / 25-28, arrows CC 114 up, 115 down, 116 left, 117 right; channel = template (0-7 user,
 * 8-15 factory). LED index for the SysEx: pads 0-7, buttons 8-11. */
enum { LC_UP = 114, LC_DOWN = 115, LC_LEFT = 116, LC_RIGHT = 117 };
/* LED values: 16 * green + red + 12 (copy + clear) */
enum { LED_OFF = 0x0C, LED_RED_LO = 0x0D, LED_RED = 0x0F, LED_GREEN_LO = 0x1C, LED_GREEN = 0x3C,
       LED_AMBER = 0x3F, LED_AMBER_LO = 0x1D };

struct Map {
    bool used = false;              /* has a source */
    uint8_t dev = DEV_ANY, sch = 0, stype = S_CC, snum = 0;
    uint8_t page = 0;               /* 0 = all pages, 1..4 */
    uint8_t out = 0, ch = 0, type = D_OFF, num = 0, lo = 0, hi = 127, btn = B_MOMENT;
    uint8_t enc = ENC_ABS;
    int16_t val = -1;               /* where the control stands, 0..127; -1 = not known yet */
};
struct Run { int last = -1; bool high = false, on = false; int note = -1; bool caught = false; int prev = -1; };
struct Settings {
    int page = 0;      /* active page 0..3 */
    int follow = 1;    /* touching a mapped control selects its slot in the editor */
    int leds = 1;      /* drive the Launch Control's LEDs */
    int thru = 0;      /* unmapped controls: 0 drop, 1 pass to Out A unchanged */
    int link = 0;      /* Out A also feeds every other plugin's "Control In" port */
    int input = 0;     /* 0 Launch Control + UC-33 only, 1 every USB controller */
    int arrows = 0;    /* Launch Control arrows: 0 pages/slots, 1 free to map */
    int feedback = 1;  /* send positions back: 0 off, 1 to the EC4, 2 to the EC4 and every other USB controller */
    int takeover = 1;  /* absolute pots without feedback: 0 jump, 1 pickup */
};
struct Msg { uint8_t port, len, d[3]; };

/* changed[] bits, read and cleared by the wrapper */
enum { CH_SEL = 1, CH_LEARN = 2, CH_PAGE = 4, CH_LAST = 8, CH_SLOT = 16 };

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int lc_pad_index(int note) {
    if (note >= 9 && note <= 12) return note - 9;
    if (note >= 25 && note <= 28) return note - 25 + 4;
    return -1;
}

/* "LC U1 KNOB 5", "LC F2 PAD 3", "UC CH3 CC 7", "USB CH10 NOTE 36" */
static inline void src_name(char *b, size_t n, int dev, int ch, int stype, int num) {
    char pre[24];
    if (dev == DEV_LC) std::snprintf(pre, sizeof pre, "LC %c%d", ch < 8 ? 'U' : 'F', (ch & 7) + 1);
    else std::snprintf(pre, sizeof pre, "%s CH%d", dev == DEV_UC ? "UC" : dev == DEV_USB ? "USB" : dev == DEV_EC4 ? "EC4" : "ANY", ch + 1);
    if (dev == DEV_LC && stype == S_CC) {
        if (num >= 21 && num <= 28) { std::snprintf(b, n, "%s KNOB %d", pre, num - 20); return; }
        if (num >= 41 && num <= 48) { std::snprintf(b, n, "%s KNOB %d", pre, num - 32); return; }
        static const char *const AR[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
        if (num >= LC_UP && num <= LC_RIGHT) { std::snprintf(b, n, "%s %s", pre, AR[num - LC_UP]); return; }
    }
    if (dev == DEV_LC && stype == S_NOTE && lc_pad_index(num) >= 0) {
        std::snprintf(b, n, "%s PAD %d", pre, lc_pad_index(num) + 1);
        return;
    }
    std::snprintf(b, n, "%s %s %d", pre, stype == S_NOTE ? "NOTE" : "CC", num);
}

class Mapper {
public:
    Map map[NMAP];
    Run run[NMAP];
    Settings s;
    int sel = 0;             /* slot shown in the editor */
    bool learn = false;
    int lc_ch = 0;           /* the Launch Control's current template = its channel */
    char last[96] = "-";
    int changed = 0;
    Msg q[QMAX];
    int nq = 0;
    long dropped = 0;

    void clear_slot(int i) { map[i] = Map(); run[i] = Run(); }
    void reset() {
        for (int i = 0; i < NMAP; i++) clear_slot(i);
        s = Settings();
        sel = 0; learn = false;
        changed |= CH_SEL | CH_PAGE | CH_LEARN | CH_SLOT;
    }

    /* ---- output queue ---------------------------------------------------- */
    void emit(int port, int st, int d1, int d2, int len = 3) {
        if (nq >= QMAX) { dropped++; return; }
        Msg &m = q[nq++];
        m.port = (uint8_t)(port >= FB_PORT ? clampi(port, FB_PORT, FB_PORT + NDEV - 1) : clampi(port, 0, NOUTS - 1));
        m.len = (uint8_t)len;
        m.d[0] = (uint8_t)st; m.d[1] = (uint8_t)(d1 & 0x7f); m.d[2] = (uint8_t)(d2 & 0x7f);
    }

    /* ---- pages: held notes must not hang when their slot leaves the page ---- */
    void release_all() {
        for (int i = 0; i < NMAP; i++) {
            Run &r = run[i];
            if (r.note >= 0) { emit(map[i].out, 0x80 | map[i].ch, r.note, 0); r.note = -1; }
            r.high = false;
        }
    }
    /* positions are taken up afresh (PICKUP), e.g. after the page changed under the pots */
    void uncatch() { for (int i = 0; i < NMAP; i++) { run[i].caught = false; run[i].prev = -1; } }
    bool fb_capable(int dev) const {
        return (s.feedback >= 1 && dev == DEV_EC4) || (s.feedback == 2 && dev == DEV_USB);
    }
    /* FEEDBACK: every absolute CC slot of the page tells its controller where it stands (a control
     * used by several slots of the page: the first one). Call on page change, load, device found. */
    void feedback_all(int only_dev = -1) {
        if (!s.feedback) return;
        for (int i = 0; i < NMAP; i++) {
            const Map &m = map[i];
            if (!m.used || m.stype != S_CC || m.enc != ENC_ABS || m.val < 0 || !on_page(m) || !fb_capable(m.dev)) continue;
            if (only_dev >= 0 && m.dev != only_dev) continue;
            bool dup = false;
            for (int j = 0; j < i && !dup; j++) {
                const Map &o = map[j];
                dup = o.used && o.stype == S_CC && o.enc == ENC_ABS && o.val >= 0 && on_page(o) && o.dev == m.dev &&
                      o.sch == m.sch && o.snum == m.snum;
            }
            if (!dup) emit(FB_PORT + m.dev, 0xB0 | m.sch, m.snum, m.val);
        }
    }
    void set_page(int p) {
        p = clampi(p, 0, NPAGES - 1);
        if (p == s.page) return;
        release_all();
        s.page = p;
        uncatch();
        feedback_all();
        changed |= CH_PAGE;
    }
    void select(int i) {
        i = clampi(i, 0, NMAP - 1);
        if (i != sel) { sel = i; changed |= CH_SEL; }
    }

    bool on_page(const Map &m) const { return m.page == 0 || m.page == s.page + 1; }
    bool matches(const Map &m, int dev, int ch, int stype, int num) const {
        return m.used && (m.dev == DEV_ANY || m.dev == dev) && m.sch == ch && m.stype == stype && m.snum == num;
    }

    /* ---- one slot reacts to its control ---------------------------------- */
    void drive(int i, int in) {   /* in 0..127 (a note: 127 pressed, 0 released) */
        const Map &m = map[i];
        Run &r = run[i];
        const bool high = in >= 64, rise = high && !r.high;
        r.high = high;
        if (m.type == D_PAGE) {
            if (rise) set_page(m.num == 0 ? (s.page + 1) % NPAGES : m.num - 1);
            return;
        }
        if (m.btn == B_TOGGLE) {
            if (!rise) return;
            r.on = !r.on;
            in = r.on ? 127 : 0;
        }
        const int v = clampi((int)std::lround(m.lo + (m.hi - m.lo) * (in / 127.0)), 0, 127);
        switch (m.type) {
        case D_CC:
            if (v != r.last) { emit(m.out, 0xB0 | m.ch, m.num, v); r.last = v; }
            break;
        case D_PROG:
            if (v != r.last) { emit(m.out, 0xC0 | m.ch, v, 0, 2); r.last = v; }
            break;
        case D_NOTE: {
            const bool want = in >= 64;
            if (want && r.note < 0) { emit(m.out, 0x90 | m.ch, m.num, v > 0 ? v : 1); r.note = m.num; }
            else if (!want && r.note >= 0) { emit(m.out, 0x80 | m.ch, r.note, 0); r.note = -1; }
            break;
        }
        default: break;
        }
    }

    /* ---- input ------------------------------------------------------------- */
    void input(int dev, int st, int d1, int d2) {
        const int type = st & 0xF0, ch = st & 0x0F;
        d1 &= 0x7f; d2 &= 0x7f;
        int stype, in;
        bool press;
        if (type == 0xB0) { stype = S_CC; in = d2; press = d2 > 0; }
        else if (type == 0x90 && d2 > 0) { stype = S_NOTE; in = 127; press = true; }
        else if (type == 0x80 || type == 0x90) { stype = S_NOTE; in = 0; press = false; }
        else {   /* pitch bend, aftertouch, program change: only ever passed through */
            if (s.thru) emit(0, st, d1, d2, (type == 0xC0 || type == 0xD0) ? 2 : 3);
            return;
        }
        if (dev == DEV_LC) lc_ch = ch;
        char nm[48];
        src_name(nm, sizeof nm, dev, ch, stype, d1);
        if (stype == S_CC) std::snprintf(last, sizeof last, "%s = %d", nm, d2);
        else std::snprintf(last, sizeof last, "%s %s", nm, press ? "ON" : "OFF");
        changed |= CH_LAST;

        /* Launch Control arrows: left/right = page, up/down = editor slot */
        if (dev == DEV_LC && s.arrows == 0 && stype == S_CC && d1 >= LC_UP && d1 <= LC_RIGHT) {
            if (!press) return;
            if (d1 == LC_LEFT) set_page(s.page - 1);
            else if (d1 == LC_RIGHT) set_page(s.page + 1);
            else select(sel + (d1 == LC_UP ? -1 : 1));
            return;
        }
        if (learn) {
            if (stype == S_NOTE && !press) return;   /* the release of the learning pad */
            Map &m = map[sel];
            m.used = true; m.dev = (uint8_t)dev; m.sch = (uint8_t)ch; m.stype = (uint8_t)stype; m.snum = (uint8_t)d1;
            m.val = (int16_t)(stype == S_CC && m.enc == ENC_ABS ? d2 : -1);
            if (m.type == D_OFF) {   /* a fresh slot: pass the control on as it is */
                m.type = stype == S_NOTE ? D_NOTE : D_CC;
                m.out = 0; m.ch = (uint8_t)ch; m.num = (uint8_t)d1; m.lo = 0; m.hi = 127; m.btn = B_MOMENT;
            }
            run[sel] = Run();
            learn = false;
            changed |= CH_LEARN | CH_SLOT;
            return;
        }
        int first = -1, waiting = -1;
        bool moved = false;
        for (int i = 0; i < NMAP; i++) {
            Map &m = map[i];
            Run &r = run[i];
            if (!matches(m, dev, ch, stype, d1)) continue;
            /* off the page: only a release still gets through, so nothing hangs */
            if (!on_page(m) && !(r.note >= 0 && !press)) continue;
            if (first < 0) first = i;
            int v = in;
            if (stype == S_CC) {
                if (m.enc != ENC_ABS) {   /* relative: steps onto the slot's own value */
                    const int step = m.enc == ENC_REL64 ? d2 - 64 : (d2 < 64 ? d2 : d2 - 128);
                    v = clampi((m.val < 0 ? 64 : m.val) + step, 0, 127);
                } else if (s.takeover && !fb_capable(dev) && m.val >= 0 && !r.caught && in != 0 && in != 127 &&
                           m.btn == B_MOMENT && m.type != D_NOTE && m.type != D_PAGE) {
                    /* (buttons and the ends of the travel always take over at once) */
                    const bool near = std::abs(in - m.val) <= PICKUP_NEAR;
                    const bool crossed = r.prev >= 0 && ((r.prev < m.val) != (in < m.val));
                    r.prev = in;
                    if (!near && !crossed) { if (waiting < 0) waiting = m.val; continue; }
                }
                r.caught = true;
                m.val = (int16_t)v;
            }
            moved = true;
            if (m.type != D_OFF) drive(i, v);
        }
        if (first < 0) {
            if (s.thru) emit(0, st, d1, d2);
            return;
        }
        if (!moved && waiting >= 0) {   /* PICKUP: show where the pot has to go */
            std::snprintf(last, sizeof last, "%s: TO %d", nm, waiting);   /* "LC U1 KNOB 5: TO 100" */
        }
        if (s.follow) select(first);
    }

    /* Launch Control SysEx: F0 00 20 29 02 0A 77 tt F7 = the user switched to template tt */
    void lc_sysex(const uint8_t *d, int n) {
        static const uint8_t H[7] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0A, 0x77};
        if (n >= 9 && !std::memcmp(d, H, 7) && d[8] == 0xF7) { lc_ch = d[7] & 0x0F; changed |= CH_LAST; }
    }

    /* ---- Launch Control LEDs: pads show their slot, arrows the pages left/right ---- */
    void leds(uint8_t v[NLEDS]) const {
        for (int i = 0; i < NLEDS; i++) v[i] = LED_OFF;
        if (!s.leds) return;
        for (int p = 0; p < 8; p++) {
            const int note = p < 4 ? 9 + p : 25 + p - 4;
            for (int i = 0; i < NMAP; i++) {
                const Map &m = map[i];
                if (!on_page(m) || !matches(m, DEV_LC, lc_ch, S_NOTE, note)) continue;
                const Run &r = run[i];
                if (learn && i == sel) v[p] = LED_RED;
                else if (m.type == D_PAGE) v[p] = (m.num == 0 || m.num - 1 == s.page) ? LED_AMBER : LED_AMBER_LO;
                else if (m.btn == B_TOGGLE) v[p] = r.on ? LED_GREEN : LED_RED_LO;
                else v[p] = r.high ? LED_AMBER : LED_GREEN_LO;
                break;
            }
        }
        if (s.arrows == 0) {
            v[8] = v[9] = LED_RED_LO;                                  /* up / down: editor slot */
            v[10] = s.page > 0 ? LED_AMBER : LED_OFF;                  /* left */
            v[11] = s.page < NPAGES - 1 ? LED_AMBER : LED_OFF;         /* right */
        }
    }

    /* ---- state as text: settings, then one "m<slot>=..." per used or edited slot ---- */
    static bool is_default(const Map &m) {
        Map d;
        return !m.used && m.type == d.type && m.page == d.page && m.out == d.out && m.ch == d.ch &&
               m.num == d.num && m.lo == d.lo && m.hi == d.hi && m.btn == d.btn && m.enc == d.enc;
    }
    std::string state() const {
        char b[128];
        std::snprintf(b, sizeof b, "pg=%d;fo=%d;le=%d;th=%d;li=%d;in=%d;ar=%d;fb=%d;to=%d;",
                      s.page, s.follow, s.leds, s.thru, s.link, s.input, s.arrows, s.feedback, s.takeover);
        std::string t = b;
        for (int i = 0; i < NMAP; i++) {
            const Map &m = map[i];
            if (is_default(m)) continue;
            std::snprintf(b, sizeof b, "m%d=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d;", i + 1, (int)m.used, m.dev, m.sch,
                          m.stype, m.snum, m.page, m.out, m.ch, m.type, m.num, m.lo, m.hi, m.btn, m.enc, (int)m.val);
            t += b;
        }
        return t;
    }
    /* unknown keys are skipped (a wrapper's own keys like "prog=" live in the same text) */
    void apply(const std::string &t) {
        release_all();
        reset();
        for (size_t pos = 0; pos < t.size();) {
            size_t semi = t.find(';', pos);
            if (semi == std::string::npos) break;
            std::string kv = t.substr(pos, semi - pos);
            pos = semi + 1;
            size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const char *v = kv.c_str() + eq + 1;
            const int x = std::atoi(v);
            if (k == "pg") s.page = clampi(x, 0, NPAGES - 1);
            else if (k == "fo") s.follow = x != 0;
            else if (k == "le") s.leds = x != 0;
            else if (k == "th") s.thru = x != 0;
            else if (k == "li") s.link = x != 0;
            else if (k == "in") s.input = x != 0;
            else if (k == "ar") s.arrows = x != 0;
            else if (k == "fb") s.feedback = clampi(x, 0, 2);
            else if (k == "to") s.takeover = x != 0;
            else if (k.size() > 1 && k[0] == 'm') {
                const int i = std::atoi(k.c_str() + 1) - 1;
                int f[15] = {0};
                f[14] = -1;   /* 0.1.0 maps had 13 fields: absolute, position unknown */
                if (i < 0 || i >= NMAP) continue;
                if (std::sscanf(v, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                                &f[6], &f[7], &f[8], &f[9], &f[10], &f[11], &f[12], &f[13], &f[14]) < 13) continue;
                Map &m = map[i];
                m.used = f[0] != 0;
                m.dev = (uint8_t)clampi(f[1], 0, NDEV - 1); m.sch = (uint8_t)clampi(f[2], 0, 15);
                m.stype = (uint8_t)clampi(f[3], 0, 1); m.snum = (uint8_t)clampi(f[4], 0, 127);
                m.page = (uint8_t)clampi(f[5], 0, NPAGES); m.out = (uint8_t)clampi(f[6], 0, NOUTS - 1);
                m.ch = (uint8_t)clampi(f[7], 0, 15); m.type = (uint8_t)clampi(f[8], 0, 4);
                m.num = (uint8_t)clampi(f[9], 0, 127); m.lo = (uint8_t)clampi(f[10], 0, 127);
                m.hi = (uint8_t)clampi(f[11], 0, 127); m.btn = (uint8_t)clampi(f[12], 0, 1);
                m.enc = (uint8_t)clampi(f[13], 0, 2); m.val = (int16_t)clampi(f[14], -1, 127);
            }
        }
        feedback_all();   /* the controllers show the loaded positions */
        changed |= CH_SEL | CH_PAGE | CH_LEARN | CH_SLOT;
    }
};

}   // namespace ccmap
#endif
