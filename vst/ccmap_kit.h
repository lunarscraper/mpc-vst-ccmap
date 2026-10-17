/* =============================================================================
 * ccmap_kit.h - the BAUKASTEN (control kit) of CC Map, free of VST, ALSA and HTTP.
 *
 * Pages of on-screen controls the user puts together on the phone / iPad: every element sends one
 * MIDI CC (an XY pad: two) on its own output port (Out A..D) and channel, scaled to MIN..MAX.
 *
 *   FADER   vertical, the touch position is the value
 *   KNOB    wipe up / down (relative, no jump)
 *   BUTTON  MAX while held, MIN on release
 *   TOGGLE  every tap switches MIN / MAX
 *   XY      a square pad: X = CC, Y = CC 2
 *   PICKER  shows its value; a tap opens a roll to turn up / down: the values MIN..MAX in steps of STEP,
 *           optionally with names ("Saw,Square,Noise"); sends the chosen value itself
 *
 * The kit is one library for every project (a text file next to the presets, written on every edit,
 * never on a value move). Values live in memory only: after a restart a control shows "-" until it
 * is moved. A page lays its elements out on a grid of COLS columns; an element is W columns wide and
 * H rows high, in list order.
 * ========================================================================== */
#ifndef CCMAP_KIT_H
#define CCMAP_KIT_H
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ccmap_core.h"
#include "forcehub.h"

namespace cckit {

enum Type { T_FADER = 0, T_KNOB = 1, T_BUTTON = 2, T_TOGGLE = 3, T_XY = 4, T_PICKER = 5, NTYPES = 6 };
enum { MAXPAGES = 16, MAXEL = 48, NCOLORS = 8, NAMELEN = 24, LABELSLEN = 600 };

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* printable, no tabs / line breaks, at most NAMELEN bytes without cutting a UTF-8 character */
static inline std::string clean(const std::string &s, size_t maxlen = NAMELEN) {
    std::string o;
    for (char c : s) {
        const unsigned char u = (unsigned char)c;
        if (u < 32 || u == 127) continue;
        o += c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    size_t st = 0;
    while (st < o.size() && o[st] == ' ') st++;
    o = o.substr(st);
    if (o.size() > maxlen) {
        size_t n = maxlen;
        while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) n--;   /* o[n] continues a character: cut before it */
        o.resize(n);
    }
    return o;
}

struct El {
    int type = T_FADER;
    std::string name;
    int color = 0;            /* 0..7, the page's palette */
    int out = 0, ch = 0;      /* Out A..D, channel 0..15 */
    int cc = 1, cc2 = 2;      /* cc2: Y of an XY pad */
    int lo = 0, hi = 127;     /* MIN > MAX inverts */
    int w = 1, h = 2;
    int step = 1;             /* PICKER: distance between two values */
    std::string labels;       /* PICKER: names of the values from MIN on, comma separated, may be empty */
    /* runtime */
    int v = -1, v2 = -1;      /* position 0..127 of the control (before MIN..MAX), -1 = unknown */
    int last = -1, last2 = -1;/* last value sent */
};
struct Page {
    std::string name = "Seite";
    int cols = 4;
    std::vector<El> el;
};

static inline const char *type_key(int t) {
    static const char *const K[NTYPES] = {"fader", "knob", "button", "toggle", "xy", "picker"};
    return K[clampi(t, 0, NTYPES - 1)];
}

struct Kit {
    std::vector<Page> pages;
    long rev = 1;   /* bumped on every edit: the pages reload the definitions */

    void sanitize(El &e) const {
        e.type = clampi(e.type, 0, NTYPES - 1);
        e.name = clean(e.name);
        e.color = clampi(e.color, 0, NCOLORS - 1);
        e.out = clampi(e.out, 0, ccmap::NOUTS - 1);
        e.ch = clampi(e.ch, 0, 15);
        e.cc = clampi(e.cc, 0, 127);
        e.cc2 = clampi(e.cc2, 0, 127);
        e.lo = clampi(e.lo, 0, 127);
        e.hi = clampi(e.hi, 0, 127);
        e.w = clampi(e.w, 1, 8);
        e.h = clampi(e.h, 1, 4);
        e.step = clampi(e.step, 1, 127);
        e.labels = clean(e.labels, LABELSLEN);
    }
    void sanitize(Page &p) const {
        p.name = clean(p.name);
        if (p.name.empty()) p.name = "Seite";
        p.cols = clampi(p.cols, 2, 8);
        if ((int)p.el.size() > MAXEL) p.el.resize(MAXEL);
        for (El &e : p.el) sanitize(e);
    }

    /* ---- sending: v (and v2) are positions 0..127; returns whether anything went out ---- */
    static int scale(const El &e, int v) { return clampi((int)std::lround(e.lo + (e.hi - e.lo) * (clampi(v, 0, 127) / 127.0)), 0, 127); }
    template <class Emit> static bool send(El &e, int v, int v2, Emit emit) {
        bool any = false;
        v = clampi(v, 0, 127);
        e.v = v;
        const int s = e.type == T_PICKER ? v : scale(e, v);   /* a picker sends the chosen value itself */
        if (e.type == T_BUTTON || s != e.last) { emit(e.out, 0xB0 | e.ch, e.cc, s); e.last = s; any = true; }
        if (e.type == T_XY && v2 >= 0) {
            v2 = clampi(v2, 0, 127);
            e.v2 = v2;
            const int s2 = scale(e, v2);
            if (s2 != e.last2) { emit(e.out, 0xB0 | e.ch, e.cc2, s2); e.last2 = s2; any = true; }
        }
        return any;
    }

    /* ---- file: "CCKIT1", then per page "P<TAB>name<TAB>cols", per element "E<TAB>type<TAB>name<TAB>
     *      color<TAB>out<TAB>channel 1..16<TAB>cc<TAB>cc2<TAB>min<TAB>max<TAB>w<TAB>h<TAB>step<TAB>labels"
     *      (step and labels since 0.9.0; older files without them load with step 1, no labels) ---- */
    std::string to_text() const {
        std::string t = "CCKIT1\n";
        char b[96];
        for (const Page &p : pages) {
            std::snprintf(b, sizeof b, "\t%d\n", p.cols);
            t += "P\t" + p.name + b;
            for (const El &e : p.el) {
                t += std::string("E\t") + type_key(e.type) + "\t" + e.name;
                std::snprintf(b, sizeof b, "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t", e.color, e.out, e.ch + 1, e.cc, e.cc2, e.lo, e.hi, e.w, e.h, e.step);
                t += b + e.labels + "\n";
            }
        }
        return t;
    }
    bool from_text(const std::string &t) {
        if (t.compare(0, 6, "CCKIT1")) return false;
        std::vector<Page> ps;
        size_t pos = 0;
        while (pos < t.size()) {
            size_t nl = t.find('\n', pos);
            if (nl == std::string::npos) nl = t.size();
            std::string line = t.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            for (size_t a = 0;;) {
                const size_t b = line.find('\t', a);
                f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            if (f[0] == "P" && f.size() >= 3 && (int)ps.size() < MAXPAGES) {
                Page p;
                p.name = f[1];
                p.cols = std::atoi(f[2].c_str());
                ps.push_back(p);
            } else if (f[0] == "E" && f.size() >= 12 && !ps.empty() && (int)ps.back().el.size() < MAXEL) {
                El e;
                e.type = T_FADER;
                for (int k = 0; k < NTYPES; k++) if (f[1] == type_key(k)) e.type = k;
                e.name = f[2];
                e.color = std::atoi(f[3].c_str());
                e.out = std::atoi(f[4].c_str());
                e.ch = std::atoi(f[5].c_str()) - 1;
                e.cc = std::atoi(f[6].c_str());
                e.cc2 = std::atoi(f[7].c_str());
                e.lo = std::atoi(f[8].c_str());
                e.hi = std::atoi(f[9].c_str());
                e.w = std::atoi(f[10].c_str());
                e.h = std::atoi(f[11].c_str());
                if (f.size() >= 13) e.step = std::atoi(f[12].c_str());
                if (f.size() >= 14) e.labels = f[13];
                ps.back().el.push_back(e);
            }
        }
        for (Page &p : ps) sanitize(p);
        pages = ps;
        rev++;
        return true;
    }

    /* ---- JSON for the page ---- */
    static std::string el_json(const El &e) {
        char b[220];
        std::snprintf(b, sizeof b, "{\"t\":\"%s\",\"c\":%d,\"o\":%d,\"ch\":%d,\"cc\":%d,\"cc2\":%d,\"lo\":%d,\"hi\":%d,\"w\":%d,\"h\":%d,\"st\":%d,\"v\":%d,\"v2\":%d,\"n\":",
                      type_key(e.type), e.color, e.out, e.ch + 1, e.cc, e.cc2, e.lo, e.hi, e.w, e.h, e.step, e.v, e.v2);
        return b + forcehub::js(e.name) + ",\"lb\":" + forcehub::js(e.labels) + "}";
    }
    std::string json() const {
        std::string j = "{\"rev\":" + std::to_string(rev) + ",\"max_pages\":" + std::to_string(MAXPAGES) +
                        ",\"max_el\":" + std::to_string(MAXEL) + ",\"pages\":[";
        for (size_t i = 0; i < pages.size(); i++) {
            const Page &p = pages[i];
            j += (i ? ",{\"name\":" : "{\"name\":") + forcehub::js(p.name) + ",\"cols\":" + std::to_string(p.cols) + ",\"el\":[";
            for (size_t k = 0; k < p.el.size(); k++) j += (k ? "," : "") + el_json(p.el[k]);
            j += "]}";
        }
        return j + "]}";
    }
    /* positions of one page only (polled while playing): {"rev":N,"v":[[v,v2],...]} */
    std::string values(int pi) const {
        std::string j = "{\"rev\":" + std::to_string(rev) + ",\"v\":[";
        if (pi >= 0 && pi < (int)pages.size()) {
            const Page &p = pages[pi];
            char b[32];
            for (size_t k = 0; k < p.el.size(); k++) {
                std::snprintf(b, sizeof b, "%s[%d,%d]", k ? "," : "", p.el[k].v, p.el[k].v2);
                j += b;
            }
        }
        return j + "]}";
    }
    /* PICKER: the name of value VAL ("" = none): the k-th name belongs to MIN + k * STEP (towards MAX) */
    static std::string label_for(const El &e, int val) {
        if (e.labels.empty()) return "";
        const int d = e.hi >= e.lo ? val - e.lo : e.lo - val;
        if (d < 0 || d % e.step || (e.hi >= e.lo ? val > e.hi : val < e.hi)) return "";
        int k = d / e.step;
        size_t a = 0;
        while (k-- > 0) { a = e.labels.find(',', a); if (a == std::string::npos) return ""; a++; }
        const size_t z = e.labels.find(',', a);
        std::string l = e.labels.substr(a, z == std::string::npos ? std::string::npos : z - a);
        while (!l.empty() && l[0] == ' ') l.erase(0, 1);
        while (!l.empty() && l.back() == ' ') l.pop_back();
        return l;
    }
    int count() const { int n = 0; for (const Page &p : pages) n += (int)p.el.size(); return n; }

    /* ---- a page from the current CC Map slots of the active page (CC targets only) ---- */
    static Page from_mapper(const ccmap::Mapper &mp, int &skipped) {
        using namespace ccmap;
        Page p;
        char b[48];
        std::snprintf(b, sizeof b, "CC Map S%d", mp.s.page + 1);
        p.name = b;
        p.cols = 4;
        skipped = 0;
        for (int i = 0; i < NMAP; i++) {
            const Map &m = mp.map[i];
            if (!m.used || !mp.on_page(m)) continue;
            if (m.type != D_CC || (int)p.el.size() >= MAXEL) { if (m.type != D_OFF) skipped++; continue; }
            El e;
            if (m.stype == S_NOTE) { e.type = m.btn == B_TOGGLE ? T_TOGGLE : T_BUTTON; e.h = 1; }
            else e.type = (m.dev == DEV_LC || has_display(m.dev)) ? T_KNOB : T_FADER;
            if (e.type == T_KNOB) e.h = 1;
            if (m.name[0]) e.name = m.name;
            else { src_name(b, sizeof b, m.dev, m.sch, m.stype, m.snum); e.name = b; }
            e.out = m.out; e.ch = m.ch; e.cc = m.num; e.cc2 = m.num < 127 ? m.num + 1 : 0; e.lo = m.lo; e.hi = m.hi;
            e.color = m.ch % NCOLORS;
            if (m.stype == S_CC) e.v = m.val;
            else e.v = mp.run[i].on ? 127 : 0;
            p.el.push_back(e);
        }
        return p;
    }
};

}   // namespace cckit
#endif
