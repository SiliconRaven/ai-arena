// Rollout-search bot: exact mirror simulator + default policy for everybody + first-action candidate search.
#include "include/sim.hpp"
#include "include/step_worker.hpp"
#include <chrono>
#include <iostream>
using namespace kbs;
using Clock = std::chrono::steady_clock;

static const double KS = (1 - 0.06) / 0.06;  // slide factor
static double g_cfg_center = 45.0;

static inline double hyp(double x, double y) { return std::sqrt(x * x + y * y); }
static inline double edgeDist(const St& s, double x, double y) { return s.half - std::max(std::fabs(x), std::fabs(y)); }
static inline double marginOf(const St& s, const Pl& p, int ahead) {
    double rx = p.x + p.vx * KS, ry = p.y + p.vy * KS;
    double h = halfAt(s.tick + ahead);
    return h - std::max(std::fabs(rx), std::fabs(ry));
}
static inline double marginWith(const St& s, double x, double y, double vx, double vy, int ahead) {
    double rx = x + vx * KS, ry = y + vy * KS;
    return halfAt(s.tick + ahead) - std::max(std::fabs(rx), std::fabs(ry));
}

// Default behaviour for any seat (used for opponents' model and my own continuation).
static Ord policy(const St& s, int me, int prefer) {
    Ord o;
    const Pl& m = s.p[me];
    if (m.out) return o;
    const bool busy = m.act != 0;
    const double hf = halfAt(s.tick + 40);
    // --- bombs: flee from the predicted blast point (the bomb keeps sliding until its fuse runs out)
    {
        double fx = 0, fy = 0; bool flee = false;
        for (int j = 0; j < s.nb; ++j) {
            const Bm& b = s.b[j];
            if (b.fuse > 70) continue;
            double k = (1 - std::pow(1 - R.bfr, (double)b.fuse)) / R.bfr;
            double ex = b.x + b.vx * k, ey = b.y + b.vy * k;
            double dEx = m.x - ex, dEy = m.y - ey, dE = hyp(dEx, dEy);
            double dNow = hyp(m.x - b.x, m.y - b.y);
            double dd = std::min(dE, dNow + 0.5);
            if (dd < R.bblast + 1.2) {
                double wgt = (R.bblast + 1.2 - dd);
                double sx = dE > dNow ? m.x - b.x : dEx, sy = dE > dNow ? m.y - b.y : dEy;
                double sl = hyp(sx, sy) + 1e-9;
                fx += sx / sl * wgt; fy += sy / sl * wgt; flee = true;
            }
        }
        if (flee) {
            double cxn = -m.x, cyn = -m.y, cl = hyp(cxn, cyn) + 1e-9;
            double w = std::max(std::fabs(m.x), std::fabs(m.y)) > hf - 4 ? 1.0 : 0.25;
            double fl = hyp(fx, fy) + 1e-9;
            setMove(o, fx / fl + w * cxn / cl, fy / fl + w * cyn / cl);
            if (m.skill == SK_DASH) o.ch = SK_DASH;
            return o;
        }
    }
    // --- edge safety
    double mg = marginOf(s, m, 36);
    if (mg < 1.6) {
        if (!busy && m.skill == SK_DASH && m.charge >= 0.05) {
            double bestm = mg, bdx = 0, bdy = 0;
            for (int k = 0; k < 16; ++k) {
                double a = k * 0.39269908169872414, dx = std::cos(a), dy = std::sin(a);
                double m2 = marginWith(s, m.x, m.y, m.vx + dx * R.dashs * m.charge, m.vy + dy * R.dashs * m.charge, 30);
                if (m2 > bestm + 0.3) { bestm = m2; bdx = dx; bdy = dy; }
            }
            if (bestm > mg + 0.3) { setDash(o, bdx, bdy); setMove(o, -m.x, -m.y); return o; }
        }
        // walk towards the centre, cancelling outward drift
        double tx = -m.x, ty = -m.y;
        setMove(o, tx - m.vx * 6, ty - m.vy * 6);
        return o;
    }
    // --- threat scan: incoming dash, loaded hammers
    bool inHam = false;
    double rtx = 0, rty = 0;
    int incoming = -1;
    for (int i = 0; i < s.n; ++i) {
        if (i == me || s.p[i].out) continue;
        const Pl& e = s.p[i];
        double ex = m.x - e.x, ey = m.y - e.y, d = hyp(ex, ey) + 1e-9;
        if (e.skill == SK_HAMMER && e.charge >= 0.25 && e.act == 0 && d < 5.0) {
            inHam = true; rtx += ex / d * (6.0 - d); rty += ey / d * (6.0 - d);
        }
        double sp = hyp(e.vx, e.vy);
        if (sp > 0.45 && d < 12.0) {
            double closing = (e.vx * (-ex) + e.vy * (-ey)) / (d * sp);
            if (closing > 0.9) incoming = i;
        }
    }
    // --- target selection
    int tgt = -1; double best = 1e18;
    for (int i = 0; i < s.n; ++i) {
        if (i == me || s.p[i].out) continue;
        double dx = s.p[i].x - m.x, dy = s.p[i].y - m.y, d2 = dx * dx + dy * dy;
        double sc = d2;
        if (i == prefer) sc *= 0.25;
        double eed = edgeDist(s, s.p[i].x, s.p[i].y);
        sc *= (0.6 + 0.4 * std::min(1.0, std::max(0.0, eed) / 6.0));
        if (sc < best) { best = sc; tgt = i; }
    }
    if (tgt < 0) return o;
    const Pl& e = s.p[tgt];
    double dx = e.x - m.x, dy = e.y - m.y, d = hyp(dx, dy);
    double ux = d > 0 ? dx / d : 1, uy = d > 0 ? dy / d : 0;
    const double rcen = hyp(m.x, m.y);
    const double cl = rcen + 1e-9;

    // --- hammer release: when it hits, kill shot, or before the charge times out
    if (!busy && m.skill == SK_HAMMER && m.charge > 0) {
        int bi = -1; double bscore = -1e9;
        for (int i = 0; i < s.n; ++i) {
            if (i == me || s.p[i].out) continue;
            const Pl& q = s.p[i];
            double qx = q.x - m.x, qy = q.y - m.y, qd = hyp(qx, qy);
            if (qd > R.hreach - 0.2) continue;
            double vx = q.vx + qx / (qd + 1e-9) * R.hpow * m.charge, vy = q.vy + qy / (qd + 1e-9) * R.hpow * m.charge;
            double qm = marginWith(s, q.x, q.y, vx, vy, 24);
            double sc = -qm + (qm < 0 ? 5 : 0);
            if (sc > bscore) { bscore = sc; bi = i; }
        }
        if (bi >= 0) {
            const Pl& q = s.p[bi];
            double qx = q.x - m.x, qy = q.y - m.y, qd = hyp(qx, qy);
            double vx = q.vx + qx / (qd + 1e-9) * R.hpow * m.charge, vy = q.vy + qy / (qd + 1e-9) * R.hpow * m.charge;
            double qm = marginWith(s, q.x, q.y, vx, vy, 24);
            if (qm < 0 || m.charge >= 0.35 || m.held >= 30 || m.bar <= 0.001) { setHammer(o, qx, qy); setMove(o, qx, qy); return o; }
        }
        if (m.held >= 36) {  // dump the charge before it times out; recoil nudges us away from the nearest enemy
            setHammer(o, ux, uy);
            setMove(o, -m.x, -m.y);
            return o;
        }
    }
    // --- incoming dash: dodge sideways if we hold a dash
    if (incoming >= 0 && !busy && m.skill == SK_DASH && m.charge >= 0.2) {
        const Pl& q = s.p[incoming];
        double sp = hyp(q.vx, q.vy), qx = q.vx / sp, qy = q.vy / sp;
        double px = -qy, py = qx;
        double m1 = marginWith(s, m.x, m.y, m.vx + px * R.dashs * m.charge, m.vy + py * R.dashs * m.charge, 30);
        double m2 = marginWith(s, m.x, m.y, m.vx - px * R.dashs * m.charge, m.vy - py * R.dashs * m.charge, 30);
        if (m2 > m1) { px = -px; py = -py; }
        setDash(o, px, py); o.ch = SK_DASH;
        return o;
    }
    // --- dash release
    if (!busy && m.skill == SK_DASH && m.charge > 0 && d < 12.5) {
        bool killshot = false;
        if (m.charge >= 0.35) {
            double ev = e.vx + ux * 0.95 * 0.8 * R.dashs * m.charge, evy = e.vy + uy * 0.95 * 0.8 * R.dashs * m.charge;
            killshot = marginWith(s, e.x, e.y, ev, evy, 24) < -0.5 && d > 2.0;
        }
        if (m.charge >= 0.6 || killshot || m.held >= 30 || (m.bar <= 0.001 && m.charge >= 0.25)) {
            double tau = std::min(18.0, d / (0.8 * R.dashs * m.charge + 0.05));
            double slide = (1 - std::pow(0.94, tau)) / 0.06;
            double px = e.x + e.vx * slide, py = e.y + e.vy * slide;
            double qx = px - m.x, qy = py - m.y, ql = hyp(qx, qy) + 1e-9;
            double missm = marginWith(s, m.x, m.y, m.vx + qx / ql * R.dashs * m.charge, m.vy + qy / ql * R.dashs * m.charge, 30);
            if (d <= 4.0 || missm >= 0.2) {
                setDash(o, px - m.x, py - m.y);
                setMove(o, ux, uy);
                return o;
            }
        }
    }
    // --- bombs: lob one at the target whenever it is ready and the target is in a useful range
    auto lobBomb = [&](Ord& oo) {
        if (!busy && m.bomb_cd <= 0 && d >= 4.2 && d <= 11.0 && edgeDist(s, m.x, m.y) > 3.0) {
            double L = std::min(d, 9.4), f = std::log(std::max(0.05, 1.0 - L / 10.0)) / std::log(0.95);
            int fuse = (int)std::lround(std::max((double)R.bfmin, std::min(60.0, f)));
            setBomb(oo, ux + e.vx * 3 * 0.0, uy, fuse);
            double px = e.x + e.vx * 6 - m.x, py = e.y + e.vy * 6 - m.y;
            setBomb(oo, px, py, fuse);
        }
    };
    // --- retreat from a loaded hammer when we are not loaded ourselves
    if (false && inHam && !busy && m.skill != SK_HAMMER) {
        double w = std::max(std::fabs(m.x), std::fabs(m.y)) > hf - 5 ? 1.5 : 0.3;
        setMove(o, rtx - w * m.x / cl, rty - w * m.y / cl);
        if (m.skill == SK_DASH) o.ch = SK_DASH;
        lobBomb(o);
        return o;
    }
    // --- jab (only when empty-handed and the hammer is not about to be ready)
    if (!busy && m.attack_cd <= 1 && d < R.areach - 0.4 && m.skill == SK_NONE && m.hammer_cd > 0) {
        setAttack(o, ux, uy);
        return o;
    }
    // --- hammer fortress: charge to full near the centre and hold it
    if (m.skill == SK_HAMMER) {
        if (m.charge < 0.999 && m.bar > 0) o.ch = SK_HAMMER;
        if (rcen > 2.5) setMove(o, -m.x, -m.y);
        else if (d < 7.0 && d > 3.0) setMove(o, ux, uy);
        lobBomb(o);
        return o;
    }
    if (m.skill == SK_DASH) {
        o.ch = SK_DASH;
        if (m.charge >= 0.99 && d > 12.5) { setMove(o, ux, uy); }
        return o;
    }
    // empty-handed: first run to the centre at full speed while nobody is close
    if (rcen > 4.5 && d > 9.0) { setMove(o, -m.x, -m.y); return o; }
    if (m.hammer_cd == 0 && m.bar >= 0.3) {
        o.ch = SK_HAMMER;
        if (rcen > 3.0) setMove(o, -m.x, -m.y);
        lobBomb(o);
        return o;
    }
    // hammer on cooldown: take position, keep bombs flying
    if (rcen > 3.5) setMove(o, -m.x, -m.y);
    else if (d > 6.0) setMove(o, ux, uy);
    lobBomb(o);
    return o;
}

struct Eval { double score; };

static double finalScore(const St& s0, const St& s, int me, double danger_acc) {
    double sc = danger_acc;
    if (!s.p[me].out) sc += 12.0 * (s.p[me].bar + s.p[me].charge);
    for (int i = 0; i < s.n; ++i) if (i != me && !s.p[i].out) sc -= 3.0 * (s.p[i].bar + s.p[i].charge);
    int enemies_alive = 0;
    for (int i = 0; i < s.n; ++i) if (i != me && !s.p[i].out) ++enemies_alive;
    const Pl& m = s.p[me];
    int alive0 = 0;
    for (int i = 0; i < s0.n; ++i) if (i != me && !s0.p[i].out) ++alive0;
    if (m.out) {
        // enemies still in at my death (those alive now plus ones that fell at the same moment or later)
        int outlast = 0;
        for (int i = 0; i < s.n; ++i) if (i != me && (!s.p[i].out || s.p[i].out_tick > m.out_tick)) ++outlast;
        sc -= 60.0 * outlast + 100.0;
        sc += 0.2 * (m.out_tick - s0.tick);
    } else {
        double mg = marginOf(s, m, 24);
        if (mg < 0) sc -= 60.0 * enemies_alive + 80.0;
        else sc -= 25.0 * std::max(0.0, 4.0 - mg);
        double r = std::max(std::fabs(m.x), std::fabs(m.y)) / std::max(1.0, s.half);
        sc -= g_cfg_center * r * r;
        { double rc = hyp(m.x, m.y); sc -= 2.5 * std::max(0.0, rc - 3.5) * std::max(0.0, rc - 3.5); }
        double ed = halfAt(s.tick + 30) - std::max(std::fabs(m.x), std::fabs(m.y));
        sc -= 5.0 * std::max(0.0, 6.0 - ed);
        if (m.skill != 0 && mg < 3.0) sc -= 15.0 * (3.0 - mg);
    }
    for (int i = 0; i < s.n; ++i) {
        if (i == me) continue;
        const Pl& q = s.p[i];
        if (s0.p[i].out) continue;
        if (q.out) {
            if (!m.out || q.out_tick <= m.out_tick) { sc += 60; if (q.out_by == me) sc += 45; }
        } else {
            double mg = marginOf(s, q, 24);
            if (mg < 0) { sc += 50; if (q.lh_by == me && s.tick - q.lh_tick < 85) sc += 35; }
            else sc += 5.0 * std::max(0.0, 4.0 - mg);
        }
    }
    if (!m.out && enemies_alive == 0) sc += 150;
    return sc;
}

static double rollout(const St& s0, int me, const Ord& first, int H, int prefer_me) {
    St s = s0;
    double acc = 0;
    Ord ords[8];
    for (int turn = 0; turn < H; ++turn) {
        for (int i = 0; i < s.n; ++i) {
            if (s.p[i].out) { ords[i] = Ord{}; continue; }
            if (i == me && turn == 0) ords[i] = first;
            else if (i != me && prefer_me == 2) ords[i] = Ord{};
            else ords[i] = policy(s, i, (i != me && prefer_me == 1) ? me : -1);
        }
        for (int k = 0; k < R.D; ++k) {
            tick(s, ords, k == 0);
            if (aliveCount(s) <= 1) break;
        }
        if (aliveCount(s) <= 1) break;
        if (!s.p[me].out) {
            double mg = marginOf(s, s.p[me], 24);
            if (mg < 2.0) acc -= 10.0 * (2.0 - mg);
        } else break;
    }
    return finalScore(s0, s, me, acc);
}

static J orderJson(const Ord& o) {
    J::object j;
    if (o.mv) j["move"] = J::array{o.mx, o.my};
    if (o.ch == SK_DASH) j["charge"] = "dash"; else if (o.ch == SK_HAMMER) j["charge"] = "hammer";
    if (o.hd) j["dash"] = J::array{o.dx, o.dy};
    if (o.hh) j["hammer"] = J::array{o.hx, o.hy};
    if (o.bo) j["bomb"] = J::object{{"dir", J::array{o.bx, o.by}}, {"fuse", o.fuse}};
    if (o.at) j["attack"] = J::array{o.ax, o.ay};
    return J(j);
}

static std::pair<J, J> decide(const J& obs, J& memory) {
    auto t0 = Clock::now();
    if (!R.loaded) loadRules(obs["rules"]);
    St s = parseObs(obs);
    int me = (int)obs["you"].as_int();
    const Pl& m = s.p[me];
    if (m.out) return {J::object{}, memory};
    const bool busy = m.act != 0;

    Ord base = policy(s, me, -1);
    std::vector<Ord> cands;
    cands.push_back(base);
    // movement variants
    for (int k = 0; k < 8; ++k) {
        double a = k * 0.7853981633974483;
        Ord c = base; c.hd = false; c.hh = false; setMove(c, std::cos(a), std::sin(a)); cands.push_back(c);
    }
    { Ord c = base; c.mv = false; c.hd = false; c.hh = false; cands.push_back(c); }
    { Ord c = base; c.ch = 0; c.hd = false; c.hh = false; cands.push_back(c); }
    if (!(m.skill == SK_HAMMER && m.charge > 0.05)) { Ord c = base; c.ch = SK_DASH; c.hd = false; c.hh = false; cands.push_back(c); }
    if (m.hammer_cd <= 1) { Ord c = base; c.ch = SK_HAMMER; c.hd = false; c.hh = false; cands.push_back(c); }
    // enemies sorted by distance
    std::vector<std::pair<double, int>> en;
    for (int i = 0; i < s.n; ++i) if (i != me && !s.p[i].out) en.push_back({hyp(s.p[i].x - m.x, s.p[i].y - m.y), i});
    std::sort(en.begin(), en.end());
    const double mg0 = marginOf(s, m, 36);
    if (!busy && m.skill == SK_DASH && m.charge > 0) {
        for (int k = 0; k < 16; ++k) {
            double a = k * 0.39269908169872414;
            if (marginWith(s, m.x, m.y, m.vx + std::cos(a) * R.dashs * m.charge, m.vy + std::sin(a) * R.dashs * m.charge, 30) < 0.2 && mg0 >= 0.2) continue;
            Ord c = base; c.hd = false; c.hh = false; setDash(c, std::cos(a), std::sin(a)); c.ch = 0; cands.push_back(c);
        }
        for (auto& pr : en) { const Pl& q = s.p[pr.second];
            { double ql = pr.first + 1e-9; double mm = marginWith(s, m.x, m.y, m.vx + (q.x - m.x) / ql * R.dashs * m.charge, m.vy + (q.y - m.y) / ql * R.dashs * m.charge, 30);
              if (pr.first > 4.0 && mm < 0.2) continue; }
            Ord c = base; c.hd = false; c.hh = false; setDash(c, q.x - m.x, q.y - m.y); c.ch = 0; cands.push_back(c);
            double tau = std::min(18.0, pr.first / (0.8 * R.dashs * m.charge + 0.05)); double slide = (1 - std::pow(0.94, tau)) / 0.06;
            Ord c2 = base; c2.hd = false; c2.hh = false; setDash(c2, q.x + q.vx * slide - m.x, q.y + q.vy * slide - m.y); c2.ch = 0; cands.push_back(c2); }
    }
    if (!busy && m.skill == SK_HAMMER && m.charge > 0) {
        for (auto& pr : en) { if (pr.first > 3.3) break; const Pl& q = s.p[pr.second]; Ord c = base; c.hd = false; c.hh = false; setHammer(c, q.x - m.x, q.y - m.y); c.ch = 0; cands.push_back(c); }
    }
    if (!busy && m.attack_cd <= 1) {
        for (auto& pr : en) { if (pr.first > 3.6) break; const Pl& q = s.p[pr.second]; Ord c = base; c.hd = false; c.hh = false; setAttack(c, q.x - m.x, q.y - m.y); cands.push_back(c); }
    }
    if (!busy && m.bomb_cd <= 0) {
        int cnt = 0;
        for (auto& pr : en) {
            if (pr.first > 10.5 || cnt >= 3) break;
            ++cnt;
            const Pl& q = s.p[pr.second];
            for (int f : {24, 30, 38, 46}) { Ord c = base; setBomb(c, q.x + q.vx * 4 - m.x, q.y + q.vy * 4 - m.y, f); cands.push_back(c); }
        }
    }

    const int H = 10;
    double bestScore = -1e18; int bi = 0;
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        double r0 = rollout(s, me, cands[ci], H, 0), r1 = rollout(s, me, cands[ci], H, 1), r2 = rollout(s, me, cands[ci], H, 2);
        double sc = 0.5 * (0.4 * r0 + 0.4 * r1 + 0.2 * r2) + 0.5 * std::min(r0, r1);
        if (ci == 0) sc += 30.0;  // prior: prefer the default policy action (less over-fitting to model errors)
        if (sc > bestScore) { bestScore = sc; bi = (int)ci; }
        double el = std::chrono::duration<double>(Clock::now() - t0).count();
        if (el > 0.045 && ci >= 8) break;
    }
    double el = std::chrono::duration<double>(Clock::now() - t0).count();
    std::cout << "t" << obs["turn"].as_int() << " c=" << cands.size() << " bi=" << bi << " sc=" << (int)bestScore << " ms=" << (int)(el * 1000) << "\n";
    return {orderJson(cands[bi]), memory};
}

int main() {
    return kb_step_worker::run([](const J& obs, J& memory) {
        try { return decide(obs, memory); }
        catch (const std::exception& e) { std::cout << "ERR " << e.what() << "\n"; return std::make_pair(J(J::object{}), memory); }
    });
}
