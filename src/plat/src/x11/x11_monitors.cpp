// X11 monitors: RandR 1.5 GetMonitors (what xrandr --listmonitors shows, so
// a video wall configured as one logical monitor is one monitor), each with
// its CRTC's refresh rate, and work areas from the WM's properties.
//
// Work areas: X11 only has per-desktop ones. Mutter/Muffin publish the
// per-monitor rectangles as _GTK_WORKAREAS_D<desktop> (the GTK3 source), and
// that is used when present; otherwise the one _NET_WORKAREA rectangle of the
// current desktop is intersected with each monitor. That approximation is
// what other toolkits (GTK) do too: a panel on one monitor of a multi-monitor setup
// shrinks _NET_WORKAREA's bounding box only where it borders the edge of the
// whole screen, so a panel between two monitors is invisible to it.
#include "x11/x11_internal.h"

#include <xcb/randr.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace plat::x11 {

namespace {

Rect intersect(const Rect &a, const Rect &b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

// Edges rounded independently, so monitors that touch in physical pixels
// still touch in logical ones.
Rect toLogical(const Rect &r, double s) {
    const int x0 = int(std::lround(r.x / s)), y0 = int(std::lround(r.y / s));
    const int x1 = int(std::lround((r.x + r.w) / s)), y1 = int(std::lround((r.y + r.h) / s));
    return {x0, y0, x1 - x0, y1 - y0};
}

bool sameRect(const Rect &a, const Rect &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool sameMonitor(const Monitor &a, const Monitor &b) {
    return a.id == b.id && a.name == b.name && sameRect(a.bounds, b.bounds) &&
           sameRect(a.workArea, b.workArea) && a.scale == b.scale &&
           a.refreshMilliHz == b.refreshMilliHz && a.primary == b.primary;
}

int modeMilliHz(const xcb_randr_mode_info_t &m) {
    if (!m.htotal || !m.vtotal)
        return 0;
    double hz = double(m.dot_clock) / (double(m.htotal) * double(m.vtotal));
    if (m.mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE)
        hz *= 2;
    if (m.mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN)
        hz /= 2;
    return int(std::lround(hz * 1000));
}

} // namespace

void X11App::setupRandr() {
    const auto *ext = xcb_get_extension_data(_c, &xcb_randr_id);
    if (!ext || !ext->present)
        return;
    Reply ver(xcb_randr_query_version_reply(_c, xcb_randr_query_version(_c, 1, 5), nullptr));
    if (!ver || ver->major_version != 1)
        return;
    _randrMinor = int(ver->minor_version);
    _randrEvent = ext->first_event;
    if (_randrMinor >= 2) {
        // Screen resizes, CRTC (mode/position) and output (connect) changes;
        // bursts of these arrive for one hotplug and are coalesced.
        xcb_randr_select_input(
            _c,
            _root,
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE
        );
    }
}

bool X11App::isWorkareaAtom(xcb_atom_t a) const {
    return a == _atoms[NetWorkarea] || a == _atoms[NetCurrentDesktop] ||
           (_gtkWorkareas && a == _gtkWorkareas);
}

void X11App::scheduleMonitorRefresh() {
    if (_monitorsPending)
        return;
    _monitorsPending = true;
    post([this] {
        _monitorsPending = false;
        readRefreshRate();
        refreshMonitors(true);
    });
}

void X11App::refreshMonitors(bool emitChange) {
    std::vector<MonitorEntry> out;

    // ── the monitors ────────────────────────────────────────────────────────
    if (_randrMinor >= 5) {
        auto  res = Reply(xcb_randr_get_screen_resources_current_reply(
            _c, xcb_randr_get_screen_resources_current(_c, _root), nullptr
        ));
        Reply mons(xcb_randr_get_monitors_reply(_c, xcb_randr_get_monitors(_c, _root, 1), nullptr));
        std::vector<xcb_atom_t> nameAtoms;
        if (mons) {
            for (auto it = xcb_randr_get_monitors_monitors_iterator(mons.p); it.rem;
                 xcb_randr_monitor_info_next(&it)) {
                const xcb_randr_monitor_info_t *mi = it.data;
                if (!mi->width || !mi->height)
                    continue;
                MonitorEntry e;
                e.phys      = {mi->x, mi->y, mi->width, mi->height};
                // The name atom is the server's own handle for the monitor
                // ("DP-1"), stable while it stays connected.
                e.m.id      = mi->name ? uint64_t(mi->name) : uint64_t(out.size() + 1);
                e.m.primary = mi->primary != 0;
                nameAtoms.push_back(mi->name);
                // Refresh from the first output's CRTC mode.
                if (res && xcb_randr_monitor_info_outputs_length(mi) > 0) {
                    const xcb_randr_output_t o = xcb_randr_monitor_info_outputs(mi)[0];
                    Reply                    oi(xcb_randr_get_output_info_reply(
                        _c, xcb_randr_get_output_info(_c, o, res->config_timestamp), nullptr
                    ));
                    if (oi && oi->crtc) {
                        Reply       ci(xcb_randr_get_crtc_info_reply(
                            _c,
                            xcb_randr_get_crtc_info(_c, oi->crtc, res->config_timestamp),
                            nullptr
                        ));
                        const auto *modes = xcb_randr_get_screen_resources_current_modes(res.p);
                        const int   n = xcb_randr_get_screen_resources_current_modes_length(res.p);
                        for (int k = 0; ci && k < n; ++k)
                            if (modes[k].id == ci->mode)
                                e.m.refreshMilliHz = modeMilliHz(modes[k]);
                    }
                }
                out.push_back(std::move(e));
            }
        }
        const auto names = atomNames(nameAtoms);
        for (size_t i = 0; i < out.size() && i < names.size(); ++i)
            out[i].m.name = names[i];
    }
    if (out.empty()) {
        // No RandR 1.5 (or no active CRTC, as on a bare Xvfb without outputs):
        // the whole screen is the one monitor. The root's live geometry, not
        // the connection setup's, which goes stale after a resize.
        MonitorEntry e;
        Reply        g(xcb_get_geometry_reply(_c, xcb_get_geometry(_c, _root), nullptr));
        e.phys             = g ? Rect{0, 0, g->width, g->height}
                               : Rect{0, 0, _screen->width_in_pixels, _screen->height_in_pixels};
        e.m.id             = 1;
        e.m.name           = "screen";
        e.m.refreshMilliHz = _refreshMilliHz; // fastest CRTC, 0 = unknown
        out.push_back(std::move(e));
    }
    // Exactly one primary: the first when the user never picked one.
    if (std::none_of(out.begin(), out.end(), [](auto &e) { return e.m.primary; }))
        out[0].m.primary = true;
    for (bool seen = false; auto &e : out) {
        e.m.primary = e.m.primary && !seen;
        seen |= e.m.primary;
    }

    // ── work areas ──────────────────────────────────────────────────────────
    uint32_t desktop = 0;
    {
        Reply r(xcb_get_property_reply(
            _c,
            xcb_get_property(_c, 0, _root, _atoms[NetCurrentDesktop], XCB_ATOM_CARDINAL, 0, 1),
            nullptr
        ));
        if (r && r->format == 32 && xcb_get_property_value_length(r.p) >= 4)
            desktop = *static_cast<uint32_t *>(xcb_get_property_value(r.p));
    }
    auto readRects = [this](xcb_atom_t prop) {
        std::vector<Rect> rects;
        if (!prop)
            return rects;
        Reply r(xcb_get_property_reply(
            _c, xcb_get_property(_c, 0, _root, prop, XCB_ATOM_CARDINAL, 0, 4096), nullptr
        ));
        if (!r || r->format != 32)
            return rects;
        const auto *v = static_cast<const uint32_t *>(xcb_get_property_value(r.p));
        const int   n = xcb_get_property_value_length(r.p) / 16;
        for (int i = 0; i < n; ++i)
            rects.push_back(
                {int(v[4 * i]), int(v[4 * i + 1]), int(v[4 * i + 2]), int(v[4 * i + 3])}
            );
        return rects;
    };
    _gtkWorkareas                      = intern("_GTK_WORKAREAS_D" + std::to_string(desktop));
    const std::vector<Rect> perMonitor = readRects(_gtkWorkareas);
    std::vector<Rect>       net        = readRects(_atoms[NetWorkarea]);
    const bool              haveNet    = !net.empty();
    const Rect              netArea    = haveNet ? net[desktop < net.size() ? desktop : 0] : Rect{};

    for (auto &e : out) {
        Rect wa = e.phys;
        if (!perMonitor.empty()) {
            // The list's order is the WM's monitor order; match by overlap.
            Rect best{};
            for (const Rect &r : perMonitor) {
                const Rect i = intersect(r, e.phys);
                if (i.w * i.h > best.w * best.h)
                    best = i;
            }
            if (best.w > 0)
                wa = best;
        } else if (haveNet) {
            const Rect i = intersect(netArea, e.phys);
            if (i.w > 0)
                wa = i;
        }
        e.m.bounds   = toLogical(e.phys, _scale);
        e.m.workArea = intersect(toLogical(wa, _scale), e.m.bounds);
        if (e.m.workArea.w <= 0)
            e.m.workArea = e.m.bounds;
        e.m.scale = _scale;
    }

    const bool changed =
        out.size() != _monitors.size() ||
        !std::equal(out.begin(), out.end(), _monitors.begin(), [](auto &a, auto &b) {
            return sameMonitor(a.m, b.m);
        });
    _monitors      = std::move(out);
    _monitorsReady = true;
    if (changed && emitChange)
        emit({.type = EventType::MonitorsChanged});
}

std::vector<Monitor> X11App::monitors() const {
    if (!_monitorsReady)
        const_cast<X11App *>(this)->refreshMonitors(false);
    std::vector<Monitor> v;
    for (auto &e : _monitors)
        v.push_back(e.m);
    return v;
}

uint64_t X11App::monitorAt(int x, int y) const {
    if (!_monitorsReady)
        const_cast<X11App *>(this)->refreshMonitors(false);
    uint64_t best  = 0;
    long     bestD = -1;
    for (auto &e : _monitors) {
        const Rect &r  = e.phys;
        // Distance from the point to the rectangle (0 inside).
        const long  dx = x < r.x ? r.x - x : x >= r.x + r.w ? x - (r.x + r.w - 1) : 0;
        const long  dy = y < r.y ? r.y - y : y >= r.y + r.h ? y - (r.y + r.h - 1) : 0;
        const long  d  = dx * dx + dy * dy;
        if (bestD < 0 || d < bestD) {
            best  = e.m.id;
            bestD = d;
        }
    }
    return best;
}

} // namespace plat::x11
