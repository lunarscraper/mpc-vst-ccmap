/* =============================================================================
 * ccmap_web.h - the phone view of CC Map on the shared Force hub (forcehub.h).
 *
 *   GET /  /info  /app        the hub, the tile info and the page (ccmap_page.h), from forcehub.h
 *   GET /state                JSON of the current map, polled by the page about 4 times a second
 *   GET /label?slot=N&name=X  give slot N (1-based) the name X ("" clears it)
 *   GET /set?slot=N&val=V     move slot N to V (0..127) as if its control had moved (knob on the phone)
 *   GET /press?slot=N&down=D  press (1) / release (0) slot N (pad on the phone)
 *   GET /files, /file, /zip   the Stems folders (ccmap_files.h), only while STEMS DOWNLOAD is on
 *
 * Nothing outside the map can be changed and nothing is ever written to disk. A download gets a
 * thread of its own (at most 2, lowest priority).
 * ========================================================================== */
#ifndef CCMAP_WEB_H
#define CCMAP_WEB_H
#include <sys/stat.h>
#include "forcehub.h"
#include "ccmap_files.h"

namespace ccweb {

using forcehub::js;

struct Hooks {
    std::function<std::string()> state;                         /* JSON, caller locks what it needs */
    std::function<void(int, const std::string &)> set_label;    /* slot index 0-based */
    std::function<bool(int, int)> remote;                       /* slot 0-based, value 0..127 */
    std::function<bool()> files_on;                             /* STEMS DOWNLOAD switch of the plugin */
};

/* the CC Map routes; returns forcehub::NOT_MINE / DONE / KEPT */
static inline int route(forcehub::Server &sv, const Hooks &h, int c, const forcehub::Request &r) {
    using namespace forcehub;
    const int slot = r.num("slot");
    if (r.route == "/state") { ok_json(c, h.state ? h.state() : "{}"); return DONE; }
    if (r.route == "/label") {
        if (slot < 1 || slot > 128 || !h.set_label) { bad(c, "slot 1..128"); return DONE; }
        h.set_label(slot - 1, r.arg("name"));
        ok_json(c, "{\"ok\":true}");
        return DONE;
    }
    if (r.route == "/set" || r.route == "/press") {
        const int v = r.route == "/set" ? r.num("val") : (r.arg("down") == "1" ? 127 : 0);
        if (slot < 1 || slot > 128 || !h.remote || !h.remote(slot - 1, v)) { bad(c, "no such slot"); return DONE; }
        ok_json(c, "{\"ok\":true}");
        return DONE;
    }
    if (r.route == "/files" || r.route == "/file" || r.route == "/zip") {
        if (!h.files_on || !h.files_on()) { reply(c, "403 Forbidden", "application/json", "{\"error\":\"off\"}"); return DONE; }
        if (r.route == "/files") {
            std::string err, j = ccfiles::list(r.arg("dir"), err);
            if (j.empty()) reply(c, "404 Not Found", "application/json", "{\"error\":" + js(err.c_str()) + "}");
            else ok_json(c, j);
            return DONE;
        }
        const bool want_dir = r.route == "/zip";
        const std::string p = ccfiles::safe(r.arg(want_dir ? "dir" : "path"));
        struct stat st;
        if (p.empty() || stat(p.c_str(), &st) || (want_dir ? !S_ISDIR(st.st_mode) : !(S_ISREG(st.st_mode) && ccfiles::is_media(p)))) {
            reply(c, "404 Not Found", "text/plain", "not available\n"); return DONE;
        }
        if (sv.downloads.load() >= 2) { reply(c, "503 Service Unavailable", "text/plain", "2 downloads running, try again\n"); return DONE; }
        sv.downloads++;
        forcehub::Server *s = &sv;
        std::thread([s, c, p, want_dir] {
            ccfiles::lowest_priority();
            timeval tv{20, 0};
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (want_dir) ccfiles::send_zip(c, p, &s->stopping); else ccfiles::send_file(c, p, &s->stopping);
            close(c);
            s->downloads--;
        }).detach();
        return KEPT;
    }
    return NOT_MINE;
}

}   // namespace ccweb
#endif
