// Arena knockback bot. Policy: counter-puncher warden. Holds a safe spot, charges hammer,
// swings at whatever can be pushed off, steals kills on doomed rivals with jabs/bombs,
// keeps a dash reserve for escapes, and plays the shrinking ring.
#include "include/step_worker.hpp"
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <chrono>

using J = mini_json::Json;

namespace {

struct Seg { double x1,y1,x2,y2; bool spring; };
struct Pl {
    double x=0,y=0,vx=0,vy=0,bar=0,charge=0;
    int skill=0;
    int pause=0, act=0, act_ticks=0;
    bool out=false;
    double aimx=0,aimy=0; bool hasaim=false;
    int bomb_cd=0, attack_cd=0, hammer_cd=0, held=0;
};
struct Bomb { int id=0; double x=0,y=0,vx=0,vy=0; int fuse=0, owner=-1; };
struct Brk { int wall=0, at=0; double x1=0,y1=0,x2=0,y2=0; };

struct G {
    int turn=1, tick=0, you=0, n=4;
    double half=16.5;
    bool has_next=false; double next_half=16.5; int next_turn=0, next_end=0;
    std::vector<Pl> pl;
    std::vector<Bomb> bombs;
    std::vector<Seg> walls;
    std::vector<Brk> broken;
    std::vector<int> kills;
    double shrink_rate=0.0;
};

double P_FRIC=0.06, P_BFRIC=0.05, P_REST=0.9;
double W_THICK=0.5, W_REST=0.8, SP_F=1.5, SP_MAX=0.5;
double P_RAD=1.0, B_RAD=0.6, B_MASS=0.5, MOVE_ACCEL=0.018;
double CHARGE_RATE=0.04, REFILL=0.02, PAUSE_TICKS=30, OVER_TICKS=45;
double HAM_REACH=3.2, HAM_POWER=0.9, HAM_CONE=0.5, HAM_RECOIL=0.15, HAM_CF=0.3;
int HAM_CD=60;
double ATK_REACH=2.8, ATK_POWER=0.35, ATK_CONE=0.5;
int ATK_WIND=9, ATK_CD=15;
double DASH_SPEED=0.9, MOVE_CF=0.2;
double BLAST_R=3.5, BLAST_P=1.4, THROW_SPEED=0.5, SPAWN_GAP=0.2;
int FUSE_MIN=24, FUSE_MAX=90, BOMB_CD=90;

inline double dbl(const J& j, const char* k) {
    if (!j.is_object() || !j.contains(k)) return 0;
    const J& v = j.at(k);
    return v.is_number() ? v.as_double() : 0;
}
inline int iget(const J& j, const char* k) {
    if (!j.is_object() || !j.contains(k)) return 0;
    const J& v = j.at(k);
    try { return v.is_int() ? (int)v.as_int() : (int)v.as_double(); } catch (...) { return 0; }
}
inline J sec(const J& r, const char* k) { return (r.is_object() && r.contains(k)) ? r.at(k) : J(); }

void load_rules(const J& r) {
    if (!r.is_object()) return;
    J p = sec(r,"physics"); if (p.is_object()) { P_FRIC=dbl(p,"friction"); P_REST=dbl(p,"restitution"); }
    J w = sec(r,"walls");   if (w.is_object()) { W_THICK=dbl(w,"thickness"); W_REST=dbl(w,"restitution"); }
    J s = sec(r,"spring");  if (s.is_object()) { SP_F=dbl(s,"factor"); SP_MAX=dbl(s,"max_speed"); }
    J q = sec(r,"player");  if (q.is_object()) P_RAD=dbl(q,"radius");
    J b = sec(r,"bomb");    if (b.is_object()) { B_RAD=dbl(b,"radius"); B_MASS=dbl(b,"mass"); P_BFRIC=dbl(b,"friction");
        THROW_SPEED=dbl(b,"throw_speed"); SPAWN_GAP=dbl(b,"spawn_gap"); BLAST_R=dbl(b,"blast_radius"); BLAST_P=dbl(b,"blast_power");
        FUSE_MIN=iget(b,"fuse_min"); FUSE_MAX=iget(b,"fuse_max"); BOMB_CD=iget(b,"cooldown_ticks"); }
    J m = sec(r,"move");    if (m.is_object()) { MOVE_ACCEL=dbl(m,"accel"); MOVE_CF=dbl(m,"charging_factor"); }
    J ba = sec(r,"bar");    if (ba.is_object()) { CHARGE_RATE=dbl(ba,"charge_rate"); REFILL=dbl(ba,"refill_rate");
        PAUSE_TICKS=iget(ba,"pause_ticks"); OVER_TICKS=iget(ba,"overcharge_ticks"); }
    J h = sec(r,"hammer");  if (h.is_object()) { HAM_REACH=dbl(h,"reach"); HAM_POWER=dbl(h,"power"); HAM_CONE=dbl(h,"cone_cos");
        HAM_RECOIL=dbl(h,"recoil"); HAM_CF=dbl(h,"charging_factor"); HAM_CD=iget(h,"cooldown_ticks"); }
    J a = sec(r,"attack");  if (a.is_object()) { ATK_REACH=dbl(a,"reach"); ATK_POWER=dbl(a,"power"); ATK_CONE=dbl(a,"cone_cos");
        ATK_WIND=iget(a,"windup_ticks"); ATK_CD=iget(a,"cooldown_ticks"); }
    J d = sec(r,"dash");    if (d.is_object()) DASH_SPEED=dbl(d,"speed");
}

inline double len(double x, double y) { return std::sqrt(x*x+y*y); }
inline void unitize(double& x, double& y) {
    double d = std::sqrt(x*x+y*y);
    if (d < 1e-12) { x = 0; y = 0; } else { x /= d; y /= d; }
}
inline double edge_margin(const G& g, double x, double y) { return g.half - std::max(std::fabs(x), std::fabs(y)); }

inline void wall_bounce(double& x, double& y, double& vx, double& vy, const Seg& w, double rad) {
    double ax = w.x1, ay = w.y1, ex = w.x2-ax, ey = w.y2-ay;
    double l = ex*ex+ey*ey;
    double t = l == 0 ? 0 : std::max(0.0, std::min(1.0, ((x-ax)*ex + (y-ay)*ey)/l));
    double qx = ax+t*ex, qy = ay+t*ey;
    double dx = x-qx, dy = y-qy, d2 = dx*dx+dy*dy;
    double reach = rad + W_THICK/2;
    if (d2 >= reach*reach) return;
    double nx, ny;
    if (d2 > 1e-18) { double d = std::sqrt(d2); nx = dx/d; ny = dy/d; }
    else { nx = -ey; ny = ex; unitize(nx, ny); }
    x = qx + nx*reach; y = qy + ny*reach;
    double inward = vx*nx + vy*ny;
    if (inward >= 0) return;
    double sp = w.spring ? std::min(SP_F*(-inward), SP_MAX) : W_REST*(-inward);
    vx += (sp-inward)*nx; vy += (sp-inward)*ny;
}

struct Fly { bool fell=false; int t=-1; double x=0,y=0,vx=0,vy=0; };

Fly fly(double x, double y, double vx, double vy, const G& g, int maxt, double fric, double rad, bool stop_on_fall = true) {
    Fly r; r.x=x; r.y=y; r.vx=vx; r.vy=vy;
    double keep = 1.0 - fric;
    for (int t = 1; t <= maxt; ++t) {
        double hh = g.half - g.shrink_rate*t;
        if (g.has_next && hh < g.next_half) hh = g.next_half;
        if (hh < 0.5) hh = 0.5;
        x += vx; y += vy;
        for (const Seg& w : g.walls) wall_bounce(x, y, vx, vy, w, rad);
        vx *= keep; vy *= keep;
        if (vx*vx+vy*vy < 1e-6) { vx = 0; vy = 0; }
        r.x=x; r.y=y; r.vx=vx; r.vy=vy;
        if (std::max(std::fabs(x), std::fabs(y)) > hh) { r.fell = true; r.t = t; if (stop_on_fall) return r; }
        if (vx == 0 && vy == 0 && t > 2) break;
    }
    return r;
}

// recorded path flight: positions[i] = position after i+1 ticks
std::vector<std::pair<double,double>> path_fly(double x, double y, double vx, double vy, const G& g, int maxt,
                                               double fric, double rad, bool stop_on_fall = true, bool* fell = nullptr) {
    std::vector<std::pair<double,double>> out;
    out.reserve(maxt);
    double keep = 1.0 - fric;
    bool f = false;
    for (int t = 1; t <= maxt; ++t) {
        double hh = g.half - g.shrink_rate*t;
        if (g.has_next && hh < g.next_half) hh = g.next_half;
        if (hh < 0.5) hh = 0.5;
        x += vx; y += vy;
        for (const Seg& w : g.walls) wall_bounce(x, y, vx, vy, w, rad);
        vx *= keep; vy *= keep;
        if (vx*vx+vy*vy < 1e-6) { vx = 0; vy = 0; }
        out.push_back({x, y});
        if (std::max(std::fabs(x), std::fabs(y)) > hh) { f = true; if (stop_on_fall) break; }
    }
    if (fell) *fell = f;
    return out;
}

inline Fly predict(const G& g, const Pl& p, int t) { return fly(p.x, p.y, p.vx, p.vy, g, t, P_FRIC, P_RAD, false); }

// value of pushing a rival from its current state to landing state f
inline double hit_value(const G& g, const Pl& e, const Fly& f) {
    if (f.fell) return 1000.0 + (60.0 - std::min(60, f.t));
    double m0 = edge_margin(g, e.x, e.y);
    double m1 = edge_margin(g, f.x, f.y);
    double v = 55.0*(m0-m1)/std::max(2.0, g.half);
    if (m1 < 2.5) v += 25.0;
    return v;
}

// ---------- parsing ----------
void parse(const J& obs, G& g) {
    g.turn = iget(obs, "turn");
    g.tick = iget(obs, "tick");
    g.you = iget(obs, "you");
    g.n = iget(obs, "n");
    if (obs.contains("rules")) load_rules(obs.at("rules"));
    const J& ar = obs.at("arena");
    g.half = dbl(ar, "half");
    g.has_next = false; g.shrink_rate = 0;
    if (ar.contains("next") && !ar.at("next").is_null()) {
        const J& nx = ar.at("next");
        g.has_next = true; g.next_half = dbl(nx,"half"); g.next_turn = iget(nx,"turn"); g.next_end = iget(nx,"end_turn");
        if (g.turn >= g.next_turn) {
            int ticks_left = std::max(1, (g.next_end - g.turn)*6);
            g.shrink_rate = std::max(0.0, g.half - g.next_half) / ticks_left;
        }
    }
    g.pl.clear();
    const J& ps = obs.at("players");
    for (size_t i = 0; i < ps.size(); ++i) {
        const J& p = ps.at(i);
        Pl q;
        q.x=dbl(p,"x"); q.y=dbl(p,"y"); q.vx=dbl(p,"vx"); q.vy=dbl(p,"vy"); q.bar=dbl(p,"bar"); q.charge=dbl(p,"charge");
        q.pause=iget(p,"pause"); q.act_ticks=iget(p,"act_ticks"); q.held=iget(p,"held");
        q.bomb_cd=iget(p,"bomb_cd"); q.attack_cd=iget(p,"attack_cd"); q.hammer_cd=iget(p,"hammer_cd");
        q.out = p.contains("out") && p.at("out").is_bool() && p.at("out").as_bool();
        if (p.contains("skill") && p.at("skill").is_string()) {
            std::string s = p.at("skill").as_string();
            q.skill = s == "dash" ? 1 : (s == "hammer" ? 2 : 0);
        }
        if (p.contains("act") && p.at("act").is_string()) {
            std::string s = p.at("act").as_string();
            q.act = s == "jab" ? 1 : (s == "jab_recover" ? 2 : (s == "recover" ? 3 : 0));
        }
        if (p.contains("aim") && p.at("aim").is_array() && p.at("aim").size() == 2) {
            q.hasaim = true; q.aimx = p.at("aim").at(0).as_double(); q.aimy = p.at("aim").at(1).as_double();
        }
        g.pl.push_back(q);
    }
    g.bombs.clear();
    if (obs.contains("bombs") && obs.at("bombs").is_array())
        for (const J& b : obs.at("bombs").as_array()) {
            Bomb bb; bb.id=iget(b,"id"); bb.x=dbl(b,"x"); bb.y=dbl(b,"y"); bb.vx=dbl(b,"vx"); bb.vy=dbl(b,"vy");
            bb.fuse=iget(b,"fuse"); bb.owner=iget(b,"owner");
            g.bombs.push_back(bb);
        }
    g.walls.clear();
    if (obs.contains("walls") && obs.at("walls").is_array())
        for (const J& w : obs.at("walls").as_array()) {
            Seg s; s.x1=dbl(w,"x1"); s.y1=dbl(w,"y1"); s.x2=dbl(w,"x2"); s.y2=dbl(w,"y2");
            s.spring = w.contains("kind") && w.at("kind").is_string() && w.at("kind").as_string() == "spring";
            g.walls.push_back(s);
        }
    g.broken.clear();
    if (obs.contains("broken") && obs.at("broken").is_array())
        for (const J& w : obs.at("broken").as_array()) {
            Brk b; b.wall=iget(w,"wall"); b.at=iget(w,"at");
            b.x1=dbl(w,"x1"); b.y1=dbl(w,"y1"); b.x2=dbl(w,"x2"); b.y2=dbl(w,"y2");
            g.broken.push_back(b);
        }
    g.kills.assign(g.pl.size(), 0);
    if (obs.contains("kills") && obs.at("kills").is_array()) {
        const J& k = obs.at("kills");
        for (size_t i = 0; i < k.size() && i < g.kills.size(); ++i) { try { g.kills[i] = (int)k.at(i).as_int(); } catch (...) {} }
    }
}

inline double seg_dist(double px, double py, double x1, double y1, double x2, double y2) {
    double ax=x1, ay=y1, ex=x2-ax, ey=y2-ay, l=ex*ex+ey*ey;
    double t = l == 0 ? 0 : std::max(0.0, std::min(1.0, ((px-ax)*ex+(py-ay)*ey)/l));
    return len(px-(ax+t*ex), py-(ay+t*ey));
}
inline double seg_dist(double px, double py, const Seg& w) { return seg_dist(px, py, w.x1, w.y1, w.x2, w.y2); }

// flight with a constant steering acceleration applied for the first `drive` ticks
Fly drive_fly(double x, double y, double vx, double vy, const G& g, int maxt, double ax, double ay, int drive) {
    Fly r; r.x=x; r.y=y; r.vx=vx; r.vy=vy;
    double keep = 1.0 - P_FRIC;
    for (int t = 1; t <= maxt; ++t) {
        double hh = g.half - g.shrink_rate*t;
        if (g.has_next && hh < g.next_half) hh = g.next_half;
        if (hh < 0.5) hh = 0.5;
        if (t <= drive) { vx += ax; vy += ay; }
        x += vx; y += vy;
        for (const Seg& w : g.walls) wall_bounce(x, y, vx, vy, w, P_RAD);
        vx *= keep; vy *= keep;
        if (vx*vx+vy*vy < 1e-6) { vx = 0; vy = 0; }
        r.x=x; r.y=y; r.vx=vx; r.vy=vy;
        if (std::max(std::fabs(x), std::fabs(y)) > hh) { r.fell = true; r.t = t; return r; }
    }
    return r;
}

// how well does a boost in direction (dx,dy) save us?  higher = safer
double escape_score(const G& g, const Pl& me, double dx, double dy, double boost) {
    double ix = -me.x, iy = -me.y; unitize(ix, iy);
    double vx = me.vx + dx*boost + ix*0.0, vy = me.vy + dy*boost;
    Fly f = drive_fly(me.x, me.y, vx, vy, g, 120, ix*MOVE_ACCEL, iy*MOVE_ACCEL, boost > 0 ? 40 : 30);
    if (f.fell) return -1000.0 + f.t*0.5;
    return edge_margin(g, f.x, f.y)*10.0;
}

J decide(const G& g, std::string& dbg) {
    J order = J::object{};
    if (g.you < 0 || g.you >= (int)g.pl.size()) return order;
    const Pl& me = g.pl[g.you];
    if (me.out) return order;
    if (me.act != 0) return order;

    std::vector<int> foe;
    for (int i = 0; i < (int)g.pl.size(); ++i) if (i != g.you && !g.pl[i].out) foe.push_back(i);
    if (foe.empty()) return order;

    double margin = edge_margin(g, me.x, me.y);
    double H = std::max(1.0, g.half);
    double rimT = std::max(3.0, std::min(8.0, H*0.50));   // margin below which we fight for the inside
    Fly myfall = fly(me.x, me.y, me.vx, me.vy, g, 100, P_FRIC, P_RAD);
    int my_fall_t = myfall.fell ? myfall.t : 999;
    int live = (int)foe.size() + 1;

    bool hold_ham = me.skill == 2 && me.charge > 0;
    bool hold_dash = me.skill == 1 && me.charge > 0;
    bool ham_ready = me.hammer_cd == 0;
    bool full_charge = me.charge >= 0.999;
    // holding any charge cuts move thrust to 0.2/0.3: at the rim that is a death sentence,
    // so dump the charge and stay mobile there
    bool rim = margin < std::min(5.5, H*0.42);
    int hold_left = OVER_TICKS - me.held;
    // charge we can no longer keep: release it this turn or lose it (and the bar) to overcharge
    bool can_refill = me.pause <= 2 && me.bar >= CHARGE_RATE;
    bool must_release = (hold_ham || hold_dash) &&
        ((full_charge && me.held >= 36) || (!full_charge && me.held >= 39 && !can_refill));

    // predicted bomb detonations
    struct Boom { double x,y; int t; int owner; };
    std::vector<Boom> booms;
    for (const Bomb& b : g.bombs) {
        Fly f = fly(b.x, b.y, b.vx, b.vy, g, std::max(1, b.fuse), P_BFRIC, B_RAD, true);
        if (!f.fell) booms.push_back({f.x, f.y, b.fuse, b.owner});
    }

    // ---- blast risk to me ----
    double blast_risk = 0, brx = 0, bry = 0;
    for (const Boom& bm : booms) {
        double d = len(me.x-bm.x, me.y-bm.y);
        if (d >= BLAST_R + 0.6) continue;
        double dd = std::max(d, 0.0001);
        double pw = BLAST_P*(1 - std::min(1.0, d/BLAST_R));
        double ux = (me.x-bm.x)/dd, uy = (me.y-bm.y)/dd;
        Fly f = fly(me.x, me.y, me.vx+ux*pw, me.vy+uy*pw, g, 80, P_FRIC, P_RAD);
        double bad = f.fell ? 1200.0 : 12.0*pw;
        bad *= bm.t <= 24 ? 1.0 : (bm.t <= 48 ? 0.6 : 0.3);
        if (bad > blast_risk) { blast_risk = bad; brx = -ux; bry = -uy; }
    }

    // ---- threat scan ----
    double threat = 0;             // 0..1+, how likely we get launched soon
    int nearest_foe = -1; double nearest_d = 1e9;
    for (int fi : foe) {
        const Pl& e = g.pl[fi];
        double d = len(e.x-me.x, e.y-me.y);
        if (d < nearest_d) { nearest_d = d; nearest_foe = fi; }
        double t = 0;
        if (e.skill == 1 && e.charge > 0.15) t += e.charge*(d < 12 ? 1.0 : 0.3);
        if (e.skill == 2 && e.charge > 0.15) t += e.charge*(d < HAM_REACH+2.5 ? 1.3 : 0.2);
        if (d < 4.0 && e.act == 0) t += 0.25;
        if (t > threat) threat = t;
    }

    // ---- best escape direction ----
    double best_esc = -1e9, esc_x = 0, esc_y = 0;
    {
        double ix = -me.x, iy = -me.y; unitize(ix, iy);
        for (int k = 0; k < 24; ++k) {
            double a = k*3.141592653589793/12.0;
            double ca = std::cos(a), sa = std::sin(a);
            double dx = ix*ca - iy*sa, dy = ix*sa + iy*ca;
            double sc = escape_score(g, me, dx, dy, hold_dash ? DASH_SPEED*me.charge : 0.0);
            if (sc > best_esc) { best_esc = sc; esc_x = dx; esc_y = dy; }
        }
    }

    // ================= emergency dash =================
    bool panic = my_fall_t <= 34 || blast_risk >= 900 || (threat >= 1.0 && margin < 4.5);
    if (panic && hold_dash && me.charge >= 0.10) {
        double bx = 0, by = 0, bsc = -1e9;
        double base_x = (blast_risk >= 900 && my_fall_t > 20) ? brx : -me.x;
        double base_y = (blast_risk >= 900 && my_fall_t > 20) ? bry : -me.y;
        unitize(base_x, base_y);
        if (base_x == 0 && base_y == 0) { base_x = 1; base_y = 0; }
        for (int k = 0; k < 32; ++k) {
            double a = k*3.141592653589793/16.0;
            double ca = std::cos(a), sa = std::sin(a);
            double dx = base_x*ca - base_y*sa, dy = base_x*sa + base_y*ca;
            double sc = escape_score(g, me, dx, dy, DASH_SPEED*me.charge);
            if (sc > bsc) { bsc = sc; bx = dx; by = dy; }
        }
        if (bsc > -400) {
            order["dash"] = J::array{bx, by};
            char buf[120]; std::snprintf(buf, sizeof buf, "ESCAPE q=%.2f fall=%d risk=%.0f m=%.1f", me.charge, my_fall_t, blast_risk, margin);
            dbg += buf;
            return order;
        }
    }

    // ================= hammer =================
    double ham_best = 0, ham_ax = 0, ham_ay = 0; bool ham_lethal = false; int ham_n = 0;
    if (hold_ham) {
        for (int ai : foe) {
            const Pl& t = g.pl[ai];
            double dx = t.x-me.x, dy = t.y-me.y;
            if (len(dx, dy) > HAM_REACH) continue;
            double ax = dx, ay = dy; unitize(ax, ay);
            double score = 0; int hits = 0; bool lethal = false;
            for (int bi : foe) {
                const Pl& e = g.pl[bi];
                double ex = e.x-me.x, ey = e.y-me.y, ed = len(ex, ey);
                if (ed > HAM_REACH) continue;
                double ux = ex, uy = ey; unitize(ux, uy);
                if (ux*ax + uy*ay < HAM_CONE) continue;
                ++hits;
                Fly f = fly(e.x, e.y, e.vx+ux*HAM_POWER*me.charge, e.vy+uy*HAM_POWER*me.charge, g, 90, P_FRIC, P_RAD);
                double v = hit_value(g, e, f);
                if (f.fell) lethal = true;
                score += v;
            }
            for (const Bomb& b : g.bombs) {
                double ex = b.x-me.x, ey = b.y-me.y, ed = len(ex, ey);
                if (ed > HAM_REACH) continue;
                double ux = ex, uy = ey; unitize(ux, uy);
                if (ux*ax + uy*ay < HAM_CONE) continue;
                // bat the bomb away: it becomes mine
                Fly fb = fly(b.x, b.y, b.vx+ux*HAM_POWER*me.charge/B_MASS, b.vy+uy*HAM_POWER*me.charge/B_MASS, g, b.fuse, P_BFRIC, B_RAD, true);
                double dme = len(fb.x-me.x, fb.y-me.y);
                score += fb.fell ? 5.0 : (dme > BLAST_R*1.5 ? 15.0 : -20.0);
            }
            Fly rf = fly(me.x, me.y, me.vx-ax*HAM_RECOIL*me.charge, me.vy-ay*HAM_RECOIL*me.charge, g, 60, P_FRIC, P_RAD);
            if (rf.fell) score -= 3000;
            else score -= 6.0*std::max(0.0, 6.0 - edge_margin(g, rf.x, rf.y));
            // defensive value: shove a charged rival that is closing in
            if (t.skill != 0 && t.charge > 0.2) {
                double d = len(dx, dy);
                if (d < 6.0 && margin < 9.0) score += 30.0*t.charge*(6.0-d)/6.0 + (margin < 5.0 ? 25.0 : 0.0);
            }
            if (hits > 0 && score > ham_best) { ham_best = score; ham_ax = ax; ham_ay = ay; ham_lethal = lethal; ham_n = hits; }
        }
        // defensive: bat an enemy bomb that is about to blow up next to me
        if (ham_best < 900) {
            for (const Bomb& b : g.bombs) {
                if (b.fuse > 26) continue;
                double dx = b.x-me.x, dy = b.y-me.y, d = len(dx, dy);
                if (d > HAM_REACH || d < 1e-9) continue;
                if (b.owner == g.you) continue;
                double ax = dx/d, ay = dy/d;
                // aim so the bomb flies away from me
                Fly fb = fly(b.x, b.y, b.vx+ax*HAM_POWER*me.charge/B_MASS, b.vy+ay*HAM_POWER*me.charge/B_MASS, g, b.fuse+10, P_BFRIC, B_RAD, true);
                double dme = len(fb.x-me.x, fb.y-me.y);
                if (dme > BLAST_R*1.4 && ham_best < 40) { ham_best = 60; ham_ax = ax; ham_ay = ay; ham_lethal = false; ham_n = 0; }
            }
        }
    }
    double ham_gate = margin < 5.0 ? 18.0 : 30.0;
    if (hold_ham && (ham_best >= 900 || (ham_best >= ham_gate && ham_n > 0) || ((must_release || rim) && me.charge >= 0.12))) {
        if (ham_n == 0) { // whiff on purpose: aim so the recoil nudges us inward
            ham_ax = esc_x; ham_ay = esc_y;
            if (ham_ax == 0 && ham_ay == 0) { ham_ax = -me.x; ham_ay = -me.y; unitize(ham_ax, ham_ay); }
            ham_ax = -ham_ax; ham_ay = -ham_ay;
        }
        order["hammer"] = J::array{ham_ax, ham_ay};
        char buf[160]; std::snprintf(buf, sizeof buf, "HAMMER q=%.2f n=%d sc=%.0f kill=%d", me.charge, ham_n, ham_best, (int)ham_lethal);
        dbg += buf;
        return order;
    }

    // ================= dash attack =================
    if (hold_dash && me.charge >= 0.2 && !panic) {
        double best = 0, bdx = 0, bdy = 0;
        for (int fi : foe) {
            const Pl& e = g.pl[fi];
            double dx = e.x-me.x, dy = e.y-me.y, d = len(dx, dy);
            if (d < 1e-6 || d > 11) continue;
            double ux = dx/d, uy = dy/d;
            double myv = len(me.vx+ux*DASH_SPEED*me.charge, me.vy+uy*DASH_SPEED*me.charge);
            // ticks until contact
            int tc = std::max(1, (int)((d - 2*P_RAD) / std::max(0.15, myv)));
            if (tc > 40) continue;
            double keep = std::pow(1.0-P_FRIC, tc);
            double speed_at = myv*keep;
            Fly ep = predict(g, e, tc);
            double cdx = ep.x-me.x, cdy = ep.y-me.y, cd = len(cdx, cdy);
            if (cd < 1e-6) continue;
            double cx = cdx/cd, cy = cdy/cd;
            double rel = speed_at*(cx*ux+cy*uy) - (e.vx*cx+e.vy*cy);
            if (rel <= 0.05) continue;
            double imp = (1+P_REST)*rel/2.0;
            Fly f = fly(ep.x, ep.y, e.vx+cx*imp, e.vy+cy*imp, g, 90, P_FRIC, P_RAD);
            double v = hit_value(g, e, f);
            // my own safety at the contact point
            double myx = ep.x - cx*2*P_RAD, myy = ep.y - cy*2*P_RAD;
            double mm = edge_margin(g, myx, myy);
            if (mm < 2.0) v -= 200;
            Fly mf = fly(myx, myy, ux*speed_at*0.1, uy*speed_at*0.1, g, 60, P_FRIC, P_RAD);
            if (mf.fell) v -= 2500;
            if (v > best) { best = v; bdx = ux; bdy = uy; }
        }
        if (best >= 900) {
            order["dash"] = J::array{bdx, bdy};
            char buf[120]; std::snprintf(buf, sizeof buf, "DASHKILL sc=%.0f", best);
            dbg += buf;
            return order;
        }
        if (must_release && best > 0 && margin > 4.0) {
            order["dash"] = J::array{bdx, bdy};
            dbg += "DASH force-hit";
            return order;
        }
    }
    if (hold_dash && (must_release || rim)) {
        double bx = esc_x, by = esc_y;
        order["dash"] = J::array{bx, by};
        dbg += "DASH reposition";
        return order;
    }
    if (hold_ham && (must_release || rim)) {
        double ax = -esc_x, ay = -esc_y;
        if (ax == 0 && ay == 0) { ax = me.x; ay = me.y; unitize(ax, ay); }
        order["hammer"] = J::array{ax, ay};
        dbg += "HAMMER dump";
        return order;
    }

    // ================= bomb =================
    // ep1: the top three threw a bomb on essentially every cooldown (8.5/game vs my 4.3) with the
    // shortest fuse, and bombs were their biggest kill source. Treat the cooldown as a budget to spend.
    if (me.bomb_cd == 0 && !panic && margin > 3.0) {
        // precompute rival paths (friction only) for the whole fuse range
        std::vector<std::vector<std::pair<double,double>>> fpath(foe.size());
        for (size_t k = 0; k < foe.size(); ++k) {
            const Pl& e = g.pl[foe[k]];
            fpath[k] = path_fly(e.x, e.y, e.vx, e.vy, g, FUSE_MAX+2, P_FRIC, P_RAD, false, nullptr);
        }
        std::vector<std::pair<double,double>> mypath = path_fly(me.x, me.y, me.vx, me.vy, g, FUSE_MAX+2, P_FRIC, P_RAD, false, nullptr);
        double best = -1e9, bdirx = 0, bdiry = 0; int bfuse = FUSE_MIN;
        for (int fi : foe) {
            const Pl& e = g.pl[fi];
            for (int lead = 0; lead <= 1; ++lead) {
                int lt = lead*15;
                Fly ep = predict(g, e, lt);
                double dx = ep.x-me.x, dy = ep.y-me.y, d = len(dx, dy);
                if (d < 1e-6) continue;
                double ux = dx/d, uy = dy/d;
                double gap = P_RAD + B_RAD + SPAWN_GAP;
                bool dead = false;
                std::vector<std::pair<double,double>> bp = path_fly(me.x+ux*gap, me.y+uy*gap,
                        me.vx+ux*THROW_SPEED, me.vy+uy*THROW_SPEED, g, FUSE_MAX, P_BFRIC, B_RAD, true, &dead);
                int lim = std::min((int)bp.size(), FUSE_MAX);
                for (int f = FUSE_MIN; f <= lim; ++f) {
                    double ex = bp[f-1].first, ey = bp[f-1].second;
                    double sc = 0;
                    for (size_t k = 0; k < foe.size(); ++k) {
                        const Pl& o = g.pl[foe[k]];
                        int idx = std::min(f-1, (int)fpath[k].size()-1);
                        double ox = fpath[k][idx].first, oy = fpath[k][idx].second;
                        double ddx = ox-ex, ddy = oy-ey, ed = len(ddx, ddy);
                        if (ed > BLAST_R) continue;
                        double pw = BLAST_P*(1-ed/BLAST_R);
                        double nx = ddx, ny = ddy;
                        if (ed < 1e-9) { nx = 1; ny = 0; } else { nx /= ed; ny /= ed; }
                        double keepf = std::pow(1.0-P_FRIC, f);
                        Fly r = fly(ox, oy, o.vx*keepf+nx*pw, o.vy*keepf+ny*pw, g, 90, P_FRIC, P_RAD);
                        sc += hit_value(g, o, r)*0.9;
                    }
                    int midx = std::min(f-1, (int)mypath.size()-1);
                    double md = len(mypath[midx].first-ex, mypath[midx].second-ey);
                    // 26% of ep1 deaths were "self": our own bomb. Coast-sim cannot see a rival bumping us
                    // into the blast afterwards, so stay out of the radius outright instead of only pricing it.
                    if (md < BLAST_R + 0.9) sc -= 1200.0;
                    if (md < BLAST_R) {
                        double pw = BLAST_P*(1-md/BLAST_R);
                        double nx = mypath[midx].first-ex, ny = mypath[midx].second-ey, nl = len(nx, ny);
                        if (nl < 1e-9) { nx = 1; ny = 0; } else { nx /= nl; ny /= nl; }
                        double keepf = std::pow(1.0-P_FRIC, f);
                        Fly r = fly(mypath[midx].first, mypath[midx].second, me.vx*keepf+nx*pw, me.vy*keepf+ny*pw, g, 90, P_FRIC, P_RAD);
                        sc -= r.fell ? 5000.0 : (margin < 6.0 ? 60.0*pw : 12.0*pw);
                    }
                    // a long fuse lets the target slide away: only pay for it if it wins more
                    sc -= 0.45*f;
                    if (sc > best) { best = sc; bdirx = ux; bdiry = uy; bfuse = f; }
                }
            }
        }
        if (best > 0 && (bdirx != 0 || bdiry != 0)) {
            order["bomb"] = J{{"dir", J::array{bdirx, bdiry}}, {"fuse", bfuse}};
            char buf[120]; std::snprintf(buf, sizeof buf, "BOMB f=%d sc=%.0f", bfuse, best);
            dbg += buf;
        }
    }

    // ================= jab =================
    if (me.attack_cd == 0 && !panic) {
        Fly myp = predict(g, me, ATK_WIND);
        double keep9 = std::pow(1.0-P_FRIC, ATK_WIND);
        double best = 0, bax = 0, bay = 0;
        for (int fi : foe) {
            const Pl& e = g.pl[fi];
            Fly ep = predict(g, e, ATK_WIND);
            double dx = ep.x-myp.x, dy = ep.y-myp.y, d = len(dx, dy);
            if (d > ATK_REACH - 0.15 || d < 1e-6) continue;
            double ux = dx/d, uy = dy/d;
            Fly f = fly(ep.x, ep.y, e.vx*keep9+ux*ATK_POWER, e.vy*keep9+uy*ATK_POWER, g, 90, P_FRIC, P_RAD);
            double conf = 1.0 - std::min(0.65, len(e.vx, e.vy)*1.6);
            if (e.act != 0 || (e.skill != 0 && e.charge > 0)) conf += 0.35;   // slow or committed
            double v = hit_value(g, e, f)*conf;
            if (v > best) { best = v; bax = ux; bay = uy; }
        }
        // a jab costs 15 ticks of thrust and nothing else: ep1's winner threw 7.3/game, I threw 0.3
        double risk = 0;
        if (margin < 3.5) risk += 40;
        if (threat >= 0.8 && nearest_d < 5.0) risk += 70;
        if (best - risk > 5 && bax != 0) {
            order["attack"] = J::array{bax, bay};
            char buf[120]; std::snprintf(buf, sizeof buf, "JAB sc=%.0f risk=%.0f", best, risk);
            dbg += buf;
        }
    }

    // ================= charge =================
    int want = 0;   // 0 none, 1 dash, 2 hammer
    bool danger = my_fall_t < 80 || blast_risk > 30 || (threat >= 0.8 && nearest_d < 7.0);
    if (panic || rim) want = hold_dash ? 1 : 0;
    else if (hold_ham) want = (danger && me.charge < 0.30) ? 1 : 2;
    else if (hold_dash) want = (!danger && ham_ready && me.charge < 0.40) ? 2 : 1;
    else want = danger ? 1 : (ham_ready ? 2 : 1);
    if (want == 2 && !ham_ready) want = hold_dash ? 1 : 0;
    if (want == 1 && margin < std::min(4.0, H*0.30) && !hold_dash) want = 0;
    // only ask to charge while the charge can still grow: that keeps `held` reset
    if (want != 0 && me.bar > 0.001 && !full_charge)
        order["charge"] = want == 1 ? J("dash") : J("hammer");

    // ================= move =================
    double sx = 0, sy = 0;
    double urg = std::max(0.0, std::min(1.0, (rimT + 1.0 - margin)/(rimT + 1.0)));
    Fly ahead = fly(me.x, me.y, me.vx, me.vy, g, 12, P_FRIC, P_RAD, false);
    double m_ahead = edge_margin(g, ahead.x, ahead.y);
    if (m_ahead < rimT) urg = std::max(urg, (rimT - m_ahead)/rimT);
    {
        double ix = -me.x, iy = -me.y; double rr = len(me.x, me.y); unitize(ix, iy);
        // ep1: the winner lived at r/H 0.41 and killed 1.7/game, I lived at 0.68 and died first.
        // Center is both the safe place and where the fights are, so pull in whenever we drift wide.
        double want_r = H*0.36;
        double cw = rr > want_r ? std::min(2.4, 3.0*(rr-want_r)/std::max(1.0, H)) : 0.0;
        double w = 0.30 + cw + 3.4*urg*urg;
        sx += ix*w; sy += iy*w;
        // brake outward drift
        double outward = me.vx*me.x + me.vy*me.y;
        if (outward > 0 && (urg > 0.15 || cw > 0.2)) { sx -= me.vx*2.0*(urg+cw*0.4); sy -= me.vy*2.0*(urg+cw*0.4); }
    }
    // rival repulsion / attraction
    // repulsion sums over every rival while the inward pull does not, so in 6/8-player games the crowd
    // out-votes centering and shoves us to the rim - exactly where every fall happens. Normalise to the
    // 4-player case that was validated, leaving that behaviour untouched.
    double rep_norm = 3.0/std::max(3.0, (double)foe.size());
    double attr_x = 0, attr_y = 0, attr_w = 0; bool attr_eager = false;
    for (int fi : foe) {
        const Pl& e = g.pl[fi];
        double dx = me.x-e.x, dy = me.y-e.y, d = len(dx, dy);
        if (d < 1e-6) { dx = 1; dy = 0; d = 1; }
        double ux = dx/d, uy = dy/d;
        double th = 0.30;
        if (e.skill == 2 && e.charge > 0 && e.hammer_cd == 0) th += 1.8*e.charge*(d < HAM_REACH+3 ? 1.5 : 0.5);
        if (e.skill == 1 && e.charge > 0) th += 1.6*e.charge*(d < 12 ? 1.2 : 0.5);
        if (e.act == 1 && e.hasaim) th += 0.4;
        if (e.attack_cd == 0 && d < 4.0) th += 0.5;
        if (margin < rimT) th *= 1.4;
        double rng = 13.0;
        if (d < rng) { double w = th*(rng-d)/rng*1.5*rep_norm; sx += ux*w; sy += uy*w; }
        // hunt: hold a hammer and a slow rival could be launched
        bool eager = hold_ham && (full_charge || me.charge >= 0.7 || me.held >= 24);
        if (hold_ham && me.charge >= 0.45 && d > HAM_REACH && d < (eager ? 11.0 : 8.0)
            && margin > std::min(6.0, H*0.45) && (eager || len(e.vx, e.vy) < 0.16)) {
            Fly f = fly(e.x, e.y, e.vx-(dx/d)*HAM_POWER*me.charge, e.vy-(dy/d)*HAM_POWER*me.charge, g, 90, P_FRIC, P_RAD);
            double v = hit_value(g, e, f);
            if (v > (eager ? 90.0 : 250.0) && v > attr_w) { attr_w = v; attr_x = -ux; attr_y = -uy; attr_eager = eager; }
        }
    }
    if (attr_w > 0 && hold_ham) { double w = attr_eager ? 2.4 : 1.5; sx += attr_x*w; sy += attr_y*w; }
    // bomb avoidance
    for (const Boom& bm : booms) {
        double dx = me.x-bm.x, dy = me.y-bm.y, d = len(dx, dy);
        double R = BLAST_R*1.7;
        if (d < R) {
            double ux = dx, uy = dy; if (d < 1e-6) { ux = 1; uy = 0; } else { ux /= d; uy /= d; }
            double w = (R-d)/R*(bm.t <= 30 ? 4.0 : 1.6);
            sx += ux*w; sy += uy*w;
        }
    }
    if (blast_risk > 40) { sx += brx*2.0; sy += bry*2.0; }
    // wall regrow zones
    for (const Brk& b : g.broken) {
        int left = b.at - g.tick;
        if (left > 120 || left < -5) continue;
        Seg s{b.x1,b.y1,b.x2,b.y2,false};
        double d = seg_dist(me.x, me.y, s);
        if (d < 3.0) {
            double ax=b.x1, ay=b.y1, ex=b.x2-ax, ey=b.y2-ay, l=ex*ex+ey*ey;
            double t = l == 0 ? 0 : std::max(0.0, std::min(1.0, ((me.x-ax)*ex+(me.y-ay)*ey)/l));
            double dx = me.x-(ax+t*ex), dy = me.y-(ay+t*ey), dd = len(dx, dy);
            if (dd < 1e-6) { dx = -me.x; dy = -me.y; dd = std::max(1e-6, len(dx, dy)); }
            double w = (3.0-d)/3.0*(left <= 96 ? 3.5 : 1.2);
            sx += dx/dd*w; sy += dy/dd*w;
        }
    }
    // standing walls: do not hug springs at speed, do not get pinned near the edge
    for (const Seg& w : g.walls) {
        double d = seg_dist(me.x, me.y, w);
        double lim = w.spring ? 2.4 : 1.7;
        if (d < lim) {
            double ax=w.x1, ay=w.y1, ex=w.x2-ax, ey=w.y2-ay, l=ex*ex+ey*ey;
            double t = l == 0 ? 0 : std::max(0.0, std::min(1.0, ((me.x-ax)*ex+(me.y-ay)*ey)/l));
            double dx = me.x-(ax+t*ex), dy = me.y-(ay+t*ey), dd = len(dx, dy);
            if (dd < 1e-6) continue;
            double sp = len(me.vx, me.vy);
            double ww = (lim-d)/lim*(w.spring ? (sp > 0.2 ? 2.2 : 0.8) : 0.7);
            sx += dx/dd*ww; sy += dy/dd*ww;
        }
    }
    // never steer outward when the rim is close
    if (margin < rimT + 1.0) {
        double w = (rimT + 1.0 - margin)/(rimT + 1.0);
        auto kill_out = [&](double nx, double ny) {
            double dot = sx*nx + sy*ny;
            if (dot > 0) { sx -= nx*dot*(1.0 + 2.5*w); sy -= ny*dot*(1.0 + 2.5*w); }
        };
        double axc = std::fabs(me.x), ayc = std::fabs(me.y);
        if (axc >= ayc) kill_out(me.x > 0 ? 1.0 : -1.0, 0.0);
        else kill_out(0.0, me.y > 0 ? 1.0 : -1.0);
        if (std::min(axc, ayc) > g.half - std::min(4.5, H*0.35)) {
            kill_out(me.x > 0 ? 1.0 : -1.0, 0.0);
            kill_out(0.0, me.y > 0 ? 1.0 : -1.0);
        }
    }
    double sl = len(sx, sy);
    if (sl < 1e-6) { sx = -me.x; sy = -me.y; sl = len(sx, sy); }
    if (sl > 1e-6) order["move"] = J::array{sx/sl, sy/sl};

    char buf[220];
    std::snprintf(buf, sizeof buf, " m=%.1f fall=%d bar=%.2f q=%.2f sk=%d held=%d want=%d risk=%.0f thr=%.1f nd=%.1f live=%d",
                  margin, my_fall_t, me.bar, me.charge, me.skill, me.held, want, blast_risk, threat, nearest_d, live);
    dbg += buf;
    return order;
}

struct State { G g; double peak=0; };
State S;

} // namespace

int main() {
    return kb_step_worker::run([](const J& obs, J& memory) {
        std::string dbg;
        J order;
        auto t0 = std::chrono::steady_clock::now();
        try {
            parse(obs, S.g);
            order = decide(S.g, dbg);
        } catch (const std::exception& e) {
            order = J::object{};
            dbg += std::string("EXC ") + e.what();
        } catch (...) {
            order = J::object{};
            dbg += "EXC";
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-t0).count();
        if (ms > S.peak) S.peak = ms;
        char tb[64]; std::snprintf(tb, sizeof tb, "|ms=%.1f pk=%.1f", ms, S.peak);
        dbg += tb;
        if (!dbg.empty()) std::cerr << dbg << "\n";
        if (memory.is_null()) memory = J::object{};
        return std::make_pair(order, memory);
    });
}
