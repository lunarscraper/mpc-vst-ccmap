/* =============================================================================
 * ccmap_files.h - audio files of the Force for the phone view (stems, recordings, exports).
 *
 *   GET /files?dir=D   JSON listing of folder D: sub-folders and audio files (name, size, date);
 *                      without dir: the Stems folders found (one per storage)
 *   GET /file?path=P   one file as a download
 *   GET /zip?dir=D     every audio file of folder D (not its sub-folders) as one ZIP, streamed
 *                      while it is read, stored without compression (WAV hardly compresses)
 *
 * Only inside a folder named "Stems" (any case) directly on a storage - /media/<card>/Stems or
 * /mnt/<drive>/Stems - and its sub-folders; only audio and MIDI files; nothing is ever written.
 * Every path is resolved (realpath) before the check, so "../" and links can't leave the folder. Off unless FILES is switched on in the plugin. A download runs on a
 * thread of its own at the lowest priority, so the page stays live and the audio isn't starved;
 * at most 2 at a time. Still: not while playing live.
 * ========================================================================== */
#ifndef CCMAP_FILES_H
#define CCMAP_FILES_H
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace ccfiles {

static const char *const STORAGE[] = {"/media", "/mnt"};

/* every <storage>/<drive>/Stems that exists, resolved */
static inline std::vector<std::string> stem_roots() {
    std::vector<std::string> out;
    for (const char *st : STORAGE) {
        DIR *d = opendir(st);
        if (!d) continue;
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string drive = std::string(st) + "/" + e->d_name;
            DIR *dd = opendir(drive.c_str());
            if (!dd) continue;
            while (dirent *f = readdir(dd)) {
                std::string n = f->d_name;
                for (char &c : n) c = (char)std::tolower((unsigned char)c);
                if (n != "stems") continue;
                char buf[PATH_MAX];
                struct stat sb;
                const std::string p = drive + "/" + f->d_name;
                if (!stat(p.c_str(), &sb) && S_ISDIR(sb.st_mode) && realpath(p.c_str(), buf) &&
                    std::find(out.begin(), out.end(), std::string(buf)) == out.end())
                    out.push_back(buf);
            }
            closedir(dd);
        }
        closedir(d);
    }
    if (const char *t = std::getenv("CCMAP_STEMS")) {   /* offline test */
        char buf[PATH_MAX];
        if (realpath(t, buf)) out.push_back(buf);
    }
    return out;
}

static inline std::string lower(std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }
static inline bool is_media(const std::string &name) {
    static const char *const EXT[] = {".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".mid", ".midi"};
    const std::string n = lower(name);
    for (const char *e : EXT) {
        const size_t l = std::strlen(e);
        if (n.size() > l && !n.compare(n.size() - l, l, e)) return true;
    }
    return false;
}
/* resolved path inside one of the Stems folders, or "" */
static inline std::string safe(const std::string &p, std::string *root_out = nullptr) {
    char buf[PATH_MAX];
    if (p.empty() || !realpath(p.c_str(), buf)) return "";
    const std::string r = buf;
    for (const std::string &root : stem_roots()) {
        if (!r.compare(0, root.size(), root) && (r.size() == root.size() || r[root.size()] == '/')) {
            if (root_out) *root_out = root;
            return r;
        }
    }
    return "";
}
static inline std::string js(const std::string &s) {
    std::string o = "\"";
    char b[8];
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 32) { std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

/* JSON listing; "" + error text on failure */
static inline std::string list(const std::string &dir, std::string &err) {
    std::string j;
    if (dir.empty()) {   /* the Stems folders themselves; one of them: open it directly */
        const std::vector<std::string> roots = stem_roots();
        if (roots.size() != 1) {
            j = "{\"dir\":\"\",\"parent\":null,\"roots\":[";
            for (size_t i = 0; i < roots.size(); i++) j += (i ? "," : "") + js(roots[i]);
            return j + "],\"dirs\":[],\"files\":[]}";
        }
        return list(roots[0], err);
    }
    std::string root;
    const std::string r = safe(dir, &root);
    if (r.empty()) { err = "folder not allowed"; return ""; }
    DIR *d = opendir(r.c_str());
    if (!d) { err = std::strerror(errno); return ""; }
    std::vector<std::string> dirs;
    std::vector<std::pair<std::string, struct stat>> files;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string p = r + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) dirs.push_back(e->d_name);
        else if (S_ISREG(st.st_mode) && is_media(e->d_name)) files.push_back({e->d_name, st});
    }
    closedir(d);
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const bool top = r == root;
    const std::string parent = top ? "" : r.substr(0, r.rfind('/'));
    j = "{\"dir\":" + js(r) + ",\"top\":" + js(root) + ",\"parent\":" + (top ? std::string("null") : js(parent)) + ",\"dirs\":[";
    for (size_t i = 0; i < dirs.size(); i++) j += (i ? "," : "") + js(dirs[i]);
    j += "],\"files\":[";
    char b[96];
    for (size_t i = 0; i < files.size(); i++) {
        std::snprintf(b, sizeof b, ",\"size\":%lld,\"mtime\":%lld}", (long long)files[i].second.st_size, (long long)files[i].second.st_mtime);
        j += std::string(i ? "," : "") + "{\"name\":" + js(files[i].first) + b;
    }
    return j + "]}";
}

/* ---- sending ---------------------------------------------------------------------------- */
static inline bool send_all(int c, const void *p, size_t n) {
    const char *s = (const char *)p;
    while (n) {
        ssize_t k = send(c, s, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        s += k; n -= (size_t)k;
    }
    return true;
}
static inline void lowest_priority() {   /* this thread only (Linux: nice is per thread) */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
}
static inline std::string attachment(const std::string &name) {   /* ASCII fallback + RFC 5987 UTF-8 */
    std::string a, u;
    char b[4];
    for (unsigned char c : name) {
        a += (c < 32 || c > 126 || c == '"' || c == '\\') ? '_' : (char)c;
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') u += (char)c;
        else { std::snprintf(b, sizeof b, "%%%02X", c); u += b; }
    }
    return "attachment; filename=\"" + a + "\"; filename*=UTF-8''" + u;
}
/* one file; returns false if the client went away */
static inline bool send_file(int c, const std::string &path, const volatile bool *stop) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    struct stat st;
    fstat(fileno(f), &st);
    const std::string name = path.substr(path.rfind('/') + 1);
    char h[512];
    std::snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
                  "Content-Disposition: %s\r\nConnection: close\r\n\r\n", (long long)st.st_size, attachment(name).c_str());
    bool ok = send_all(c, h, std::strlen(h));
    static thread_local char buf[65536];
    size_t n;
    while (ok && !*stop && (n = std::fread(buf, 1, sizeof buf, f)) > 0) ok = send_all(c, buf, n);
    std::fclose(f);
    return ok;
}

/* ---- ZIP, stored, streamed (data descriptors, so nothing has to be known in advance) ------- */
static inline uint32_t crc32(uint32_t crc, const unsigned char *p, size_t n) {
    static uint32_t T[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; }
        init = true;
    }
    crc = ~crc;
    while (n--) crc = T[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
struct ZipOut {
    int c = -1;
    uint64_t off = 0;
    bool ok = true;
    std::string central;
    int entries = 0;
    void put(const void *p, size_t n) { if (ok) ok = send_all(c, p, n); off += n; }
    static void le16(std::string &s, unsigned v) { s += (char)(v & 255); s += (char)((v >> 8) & 255); }
    static void le32(std::string &s, uint32_t v) { for (int i = 0; i < 4; i++) s += (char)((v >> (8 * i)) & 255); }
};
static inline void dos_time(time_t t, unsigned &tm_, unsigned &dt) {
    struct tm g;
    localtime_r(&t, &g);
    tm_ = (unsigned)((g.tm_hour << 11) | (g.tm_min << 5) | (g.tm_sec / 2));
    dt = (unsigned)(((g.tm_year - 80) << 9) | ((g.tm_mon + 1) << 5) | g.tm_mday);
}
static inline bool send_zip(int c, const std::string &dir, const volatile bool *stop) {
    std::vector<std::string> names;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            struct stat st;
            const std::string p = dir + "/" + e->d_name;
            if (e->d_name[0] != '.' && !stat(p.c_str(), &st) && S_ISREG(st.st_mode) && is_media(e->d_name) && st.st_size < 0xFFFFFFFFLL)
                names.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    const std::string folder = dir.substr(dir.rfind('/') + 1);
    std::string h = "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Disposition: " + attachment(folder + ".zip") +
                    "\r\nConnection: close\r\n\r\n";
    if (!send_all(c, h.data(), h.size())) return false;
    ZipOut z;
    z.c = c;
    static thread_local unsigned char buf[65536];
    for (const std::string &n : names) {
        if (*stop || !z.ok) break;
        const std::string path = dir + "/" + n;
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        struct stat st;
        fstat(fileno(f), &st);
        if (z.off + (uint64_t)st.st_size + 4096 >= 0xFFFFFFFFull) { std::fclose(f); break; }   /* 4 GB limit of plain ZIP */
        unsigned tm_, dt;
        dos_time(st.st_mtime, tm_, dt);
        const uint32_t start = (uint32_t)z.off;
        std::string lh;
        ZipOut::le32(lh, 0x04034b50); ZipOut::le16(lh, 20); ZipOut::le16(lh, 0x0808); ZipOut::le16(lh, 0);
        ZipOut::le16(lh, tm_); ZipOut::le16(lh, dt); ZipOut::le32(lh, 0); ZipOut::le32(lh, 0); ZipOut::le32(lh, 0);
        ZipOut::le16(lh, (unsigned)n.size()); ZipOut::le16(lh, 0);
        lh += n;
        z.put(lh.data(), lh.size());
        uint32_t crc = 0, size = 0;
        size_t k;
        while (z.ok && !*stop && (k = std::fread(buf, 1, sizeof buf, f)) > 0) { crc = crc32(crc, buf, k); size += (uint32_t)k; z.put(buf, k); }
        std::fclose(f);
        std::string dd;
        ZipOut::le32(dd, 0x08074b50); ZipOut::le32(dd, crc); ZipOut::le32(dd, size); ZipOut::le32(dd, size);
        z.put(dd.data(), dd.size());
        std::string& ce = z.central;
        ZipOut::le32(ce, 0x02014b50); ZipOut::le16(ce, 20); ZipOut::le16(ce, 20); ZipOut::le16(ce, 0x0808); ZipOut::le16(ce, 0);
        ZipOut::le16(ce, tm_); ZipOut::le16(ce, dt); ZipOut::le32(ce, crc); ZipOut::le32(ce, size); ZipOut::le32(ce, size);
        ZipOut::le16(ce, (unsigned)n.size()); ZipOut::le16(ce, 0); ZipOut::le16(ce, 0); ZipOut::le16(ce, 0); ZipOut::le16(ce, 0);
        ZipOut::le32(ce, 0); ZipOut::le32(ce, start);
        ce += n;
        z.entries++;
    }
    const uint32_t cd = (uint32_t)z.off;
    z.put(z.central.data(), z.central.size());
    std::string end;
    ZipOut::le32(end, 0x06054b50); ZipOut::le16(end, 0); ZipOut::le16(end, 0); ZipOut::le16(end, (unsigned)z.entries);
    ZipOut::le16(end, (unsigned)z.entries); ZipOut::le32(end, (uint32_t)z.central.size()); ZipOut::le32(end, cd); ZipOut::le16(end, 0);
    z.put(end.data(), end.size());
    return z.ok;
}

}   // namespace ccfiles
#endif
