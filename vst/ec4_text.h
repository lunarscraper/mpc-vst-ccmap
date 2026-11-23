/* =============================================================================
 * ec4_text.h - names on the Faderfox EC4's display, free of VST and ALSA (tested offline).
 *
 * Firmware 2.x of the EC4 takes text from a host over SysEx ("live SysEx"). Wire format as
 * reconstructed in Schwung (charlesvestal/schwung, src/shared/ec4_protocol.mjs, from faderfox-editor
 * and DrivenByMoss), here written anew:
 *
 *   F0 00 00 00  <commands>  F7     every command three bytes 4c 2h 1l: c = command, value = hl
 *   4E 2C 1B                        device id (EC4), first command of every message to the device
 *   4E 22 1p                        select text page p: 0 = the 16 encoder names (16 x 4 characters)
 *   4A 2h 1l                        write offset in the page (encoder n: n * 4)
 *   4D 2h 1l                        one character at the offset, the offset moves on
 *   F0 00 00 00 4E 20 10 F7         probe: the EC4 answers with its current setup and group
 *
 * Reports from the EC4 (command E, value = sub << 4 | x): sub 8 = setup x, sub 4 = group x,
 * sub A = encoder x pushed with SHIFT held, sub 6 = key x, sub E = key state x (1 down, 0 up).
 *
 * An encoder name only takes host text where the name stored in the setup is "----". Characters:
 * space, digits, A-Z, a-z and most punctuation are ASCII; $ @ [ \ ] and the umlauts are elsewhere
 * in the EC4's font, so they are replaced here. One message per encoder: 26 bytes (9 USB-MIDI
 * packets), far below the size that got damaged on the way in Schwung's tests (206 bytes).
 * ========================================================================== */
#ifndef EC4_TEXT_H
#define EC4_TEXT_H
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ec4 {

enum { NCELL = 16, CELLW = 4, MSG_LEN = 26 };

/* one character as the EC4 takes it */
static inline uint8_t char_code(unsigned char c) {
    switch (c) {
    case '$': return 'S';
    case '@': return 'a';
    case '[': return '(';
    case ']': return ')';
    case '\\': return '/';
    case '`': return '\'';
    case '{': return '(';
    case '}': return ')';
    case '|': return '!';
    case '~': return '-';
    default: break;
    }
    if (c >= 0x20 && c < 0x7F) return c;
    return '-';
}

/* UTF-8 -> plain ASCII: umlauts to their base letter, anything else outside ASCII to '-' */
static inline std::string ascii(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o += (char)c; continue; }
        if (c == 0xC3 && i + 1 < s.size()) {
            const unsigned char d = (unsigned char)s[++i];
            switch (d) {
            case 0x84: o += 'A'; break; case 0x96: o += 'O'; break; case 0x9C: o += 'U'; break;
            case 0xA4: o += 'a'; break; case 0xB6: o += 'o'; break; case 0xBC: o += 'u'; break;
            case 0x9F: o += 's'; break;
            default: o += '-'; break;
            }
            continue;
        }
        /* skip the rest of another multi-byte sequence */
        while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) i++;
        o += '-';
    }
    return o;
}

static inline bool vowel(char c) {
    c = (char)std::tolower((unsigned char)c);
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}
/* a word to n characters: first letter, then consonants, then vowels, in the original order
 * ("Cutoff" -> "Ctff", "Resonance" -> "Rsnn", "Lane" (3) -> "Lan") */
static inline std::string squeeze(const std::string &w, size_t n) {
    if (w.size() <= n) return w;
    std::vector<bool> keep(w.size(), false);
    size_t k = 0;
    keep[0] = true; k = 1;
    for (size_t i = 1; i < w.size() && k < n; i++) if (!vowel(w[i])) { keep[i] = true; k++; }
    for (size_t i = 1; i < w.size() && k < n; i++) if (!keep[i]) { keep[i] = true; k++; }
    std::string o;
    for (size_t i = 0; i < w.size(); i++) if (keep[i]) o += w[i];
    return o;
}

/* any label -> exactly 4 characters for one encoder cell, left aligned.
 * "Cutoff" -> "Ctff", "Echo Time" -> "EcTi", "Lane 12" -> "Ln12", "Osc2 Pitch" -> "OsPi", "VOL" -> "VOL " */
static inline std::string abbrev(const std::string &label) {
    const std::string a = ascii(label);
    std::vector<std::string> words;
    std::string cur;
    for (char c : a) {
        if (c == ' ' || c == '_' || c == '/' || c == '-' || c == '.' || c == ':' || c == ',') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
    std::string joined;
    for (size_t i = 0; i < words.size(); i++) joined += (i ? " " : "") + words[i];
    std::string o;
    if (words.empty()) o = "";
    else if (joined.size() <= CELLW) o = joined;
    else {
        /* a number at the end stays whole: "Lane 12", "Steps3" */
        std::string num;
        std::string &last = words.back();
        size_t d = last.size();
        while (d > 0 && std::isdigit((unsigned char)last[d - 1])) d--;
        if (d < last.size() && last.size() - d < CELLW) {
            num = last.substr(d);
            last.erase(d);
            if (last.empty()) words.pop_back();
        }
        const size_t room = CELLW - num.size();
        if (words.empty()) o = "";
        else if (words.size() == 1) o = squeeze(words[0], room);
        else {
            const size_t first = (room + 1) / 2, second = room - first;
            o = words[0].substr(0, first) + words[1].substr(0, second);
            if (o.size() < room) o = squeeze(words[0] + words[1], room);
        }
        o += num;
    }
    o = o.substr(0, CELLW);
    while (o.size() < CELLW) o += ' ';
    return o;
}

static inline void put(std::vector<uint8_t> &m, int cmd, int v) {
    m.push_back((uint8_t)(0x40 | (cmd & 0x0F)));
    m.push_back((uint8_t)(0x20 | ((v >> 4) & 0x0F)));
    m.push_back((uint8_t)(0x10 | (v & 0x0F)));
}
static inline std::vector<uint8_t> header() {
    std::vector<uint8_t> m = {0xF0, 0x00, 0x00, 0x00};
    put(m, 0xE, 0xCB);   /* 4E 2C 1B: EC4 */
    return m;
}
/* encoder cell 0..15 shows text (4 characters, padded / cut) */
static inline std::vector<uint8_t> name_msg(int cell, const std::string &text) {
    std::vector<uint8_t> m = header();
    put(m, 0xE, 0x20);                 /* page 0: encoder names */
    put(m, 0xA, (cell & 15) * CELLW);  /* offset */
    for (int i = 0; i < CELLW; i++) put(m, 0xD, char_code(i < (int)text.size() ? (unsigned char)text[(size_t)i] : ' '));
    m.push_back(0xF7);
    return m;
}
static inline std::vector<uint8_t> probe_msg() { return {0xF0, 0x00, 0x00, 0x00, 0x4E, 0x20, 0x10, 0xF7}; }

/* what one SysEx message from the EC4 reported; -1 = not in it */
struct Report { int setup = -1, group = -1, shift_push = -1; bool ok = false; };
static inline Report parse(const uint8_t *d, int n) {
    Report r;
    if (n < 8 || d[0] != 0xF0 || d[1] || d[2] || d[3] || d[n - 1] != 0xF7) return r;
    r.ok = true;
    int push = -1, state = -1;
    for (int i = 4; i + 2 < n - 0 && d[i] != 0xF7; i += 3) {
        const uint8_t a = d[i], b = d[i + 1], c = d[i + 2];
        if ((a & 0xF0) != 0x40 || (b & 0xF0) != 0x20 || (c & 0xF0) != 0x10) break;
        if ((a & 0x0F) != 0xE) continue;
        const int v = ((b & 0x0F) << 4) | (c & 0x0F), sub = v >> 4, x = v & 0x0F;
        if (sub == 0x8) r.setup = x;
        else if (sub == 0x4) r.group = x;
        else if (sub == 0xA) push = x;
        else if (sub == 0xE && push >= 0) state = x;
    }
    if (push >= 0 && state != 0) r.shift_push = push;   /* the press, not the release */
    return r;
}

/* SysEx may come in pieces (ALSA splits long ones): collect one client's bytes up to F7 */
struct Assembler {
    std::vector<uint8_t> buf;
    bool in = false;
    /* feed a piece; returns true when buf holds one whole message */
    bool feed(const uint8_t *d, int n) {
        bool done = false;
        for (int i = 0; i < n; i++) {
            if (d[i] == 0xF0) { buf.clear(); in = true; }
            if (!in) continue;
            buf.push_back(d[i]);
            if (d[i] == 0xF7) { in = false; done = true; break; }
            if (buf.size() > 512) { buf.clear(); in = false; }
        }
        return done;
    }
};

}   // namespace ec4
#endif
