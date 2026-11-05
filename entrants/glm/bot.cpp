// KNOCKOUT: engine-exact simulation policy. Survival first, opportunistic kills.
#include "include/step_worker.hpp"
#include "include/sim.hpp"
#include <chrono>
#include <cmath>
#include <cstring>

using J = mini_json::Json;
using sim::State; using sim::Order; using sim::Player; using sim::Bomb; using sim::Wall; using sim::Rules;

static const double NEG = -1e18;
static int g_you = 0;

// ---------- util ----------
static double clampd(double v,double lo,double hi){return v<lo?lo:v>hi?hi:v;}
static double lerp(double a,double b,double t){return a+(b-a)*t;}
static double hyp(double x,double y){return std::sqrt(x*x+y*y);}

// ---------- config ----------
struct Cfg {
    int horizon=16;            // sim turns for danger eval
    double self_weight=6.0;    // weight of my own fall in eval
    double enemy_fall=3.0;     // reward per enemy falling in my sim
    double kill_bonus=4.0;     // extra if I am credited
    double near_edge=-0.5;     // penalty per unit of depth beyond margin
    double edge_margin=2.0;
    double proximity=-0.15;    // per unit distance to nearest enemy when I am stronger
    double wall_closeness=-0.3;
    double bomb_danger=-2.0;   // bombs that push me toward edge
    int charge_turns=6;        // turns to charge hammer while idle
};
static Cfg cfg;

// ---------- state building ----------
static double jnum(const J&j,const char*k){return j.at(k).as_double();}
static int jint(const J&j,const char*k){return int(j.at(k).as_double()+1e-9);}

struct Snap {           // rolling snapshot for restore
    State S;
};

static State build_state(const J&obs){
    
    State S;
    const J&r=obs.at("rules");
    Rules R;
    R.player_radius=jnum(r.at("player"),"radius");
    R.bomb_radius=jnum(r.at("bomb"),"radius"); R.bomb_mass=jnum(r.at("bomb"),"mass");
    R.bomb_friction=jnum(r.at("bomb"),"friction"); R.bomb_throw_speed=jnum(r.at("bomb"),"throw_speed");
    R.bomb_spawn_gap=jnum(r.at("bomb"),"spawn_gap");
    R.friction=jnum(r.at("physics"),"friction"); R.restitution=jnum(r.at("physics"),"restitution");
    R.min_speed=jnum(r.at("physics"),"min_speed"); R.min_impulse=jnum(r.at("physics"),"min_impulse");
    R.move_accel=jnum(r.at("move"),"accel"); R.dash_charging_factor=jnum(r.at("move"),"charging_factor");
    R.hammer_charging_factor=jnum(r.at("hammer"),"charging_factor");
    R.bar_charge_rate=jnum(r.at("bar"),"charge_rate"); R.bar_refill_rate=jnum(r.at("bar"),"refill_rate");
    R.bar_pause_ticks=jint(r.at("bar"),"pause_ticks"); R.bar_overcharge_ticks=jint(r.at("bar"),"overcharge_ticks");
    R.dash_speed=jnum(r.at("dash"),"speed");
    R.hammer_reach=jnum(r.at("hammer"),"reach"); R.hammer_cone_cos=jnum(r.at("hammer"),"cone_cos");
    R.hammer_power=jnum(r.at("hammer"),"power"); R.hammer_recoil=jnum(r.at("hammer"),"recoil");
    R.hammer_recover_ticks=jint(r.at("hammer"),"recover_ticks"); R.hammer_cooldown_ticks=jint(r.at("hammer"),"cooldown_ticks");
    R.attack_reach=jnum(r.at("attack"),"reach"); R.attack_cone_cos=jnum(r.at("attack"),"cone_cos");
    R.attack_power=jnum(r.at("attack"),"power"); R.attack_recoil=jnum(r.at("attack"),"recoil");
    R.attack_windup_ticks=jint(r.at("attack"),"windup_ticks"); R.attack_recover_ticks=jint(r.at("attack"),"recover_ticks");
    R.attack_cooldown_ticks=jint(r.at("attack"),"cooldown_ticks");
    R.bomb_fuse_min=jint(r.at("bomb"),"fuse_min"); R.bomb_fuse_max=jint(r.at("bomb"),"fuse_max");
    R.bomb_cooldown_ticks=jint(r.at("bomb"),"cooldown_ticks");
    R.bomb_blast_radius=jnum(r.at("bomb"),"blast_radius"); R.bomb_blast_power=jnum(r.at("bomb"),"blast_power");
    R.walls_thickness=jnum(r.at("walls"),"thickness"); R.walls_restitution=jnum(r.at("walls"),"restitution");
    R.walls_regen_push=jnum(r.at("walls"),"regen_push"); R.walls_regen_ticks=jint(r.at("walls"),"regen_ticks");
    R.walls_warn_ticks=jint(r.at("walls"),"warn_ticks");
    R.spring_factor=jnum(r.at("spring"),"factor"); R.spring_max_speed=jnum(r.at("spring"),"max_speed");
    R.decision_ticks=jint(r,"decision_ticks"); R.max_turns=jint(r,"max_turns");
    R.precision=jint(r,"precision"); R.kill_credit_ticks=jint(r,"kill_credit_ticks");
    R.arena_half=jnum(r.at("arena"),"half");
    S.R=R;
    S.n=obs.at("players").size();
    S.D=R.decision_ticks;
    S.turn=int(obs.at("turn").as_double()+1e-9)-1;   // obs turn is 1-based
    S.tick=S.turn*S.D;
    S.half=jnum(obs.at("arena"),"half");
    // shrink plan from rules
    {
        double half=R.arena_half; double step=jnum(r.at("shrink"),"step"); double min_half=jnum(r.at("shrink"),"min_half");
        int first=jint(r.at("shrink"),"first_turn"), every=jint(r.at("shrink"),"every"), st=jint(r.at("shrink"),"shrink_turns");
        int k=0;
        while(half>min_half){
            int start=first+k*every;
            if(start>R.max_turns)break;
            double target=std::max(min_half,half-step);
            S.sh_turn.push_back(start); S.sh_end_turn.push_back(start+st-1);
            S.sh_from.push_back(half); S.sh_to.push_back(target);
            half=target; ++k;
        }
    }
    
    for(int i=0;i<S.n;++i){
        const J&p=obs.at("players").at(i);
        Player q;
        q.x=jnum(p,"x"); q.y=jnum(p,"y"); q.vx=jnum(p,"vx"); q.vy=jnum(p,"vy");
        q.bar=jnum(p,"bar"); q.charge=jnum(p,"charge");
        q.skill=p.at("skill").is_null()?0:(p.at("skill").as_string()=="dash"?1:2);
        q.pause=jint(p,"pause"); q.held=jint(p,"held");
        q.act=0;
        if(p.at("act").is_string()){
            std::string a=p.at("act").as_string();
            if(a=="recover")q.act=1; else if(a=="jab")q.act=2; else if(a=="jab_recover")q.act=3;
        }
        q.act_ticks=jint(p,"act_ticks");
        q.attack_cd=jint(p,"attack_cd"); q.hammer_cd=jint(p,"hammer_cd"); q.bomb_cd=jint(p,"bomb_cd");
        q.aimx=q.aimy=0; if(!p.at("aim").is_null()){q.aimx=p.at("aim").at(0).as_double();q.aimy=p.at("aim").at(1).as_double();}
        q.out=p.at("out").as_bool(); q.out_tick=0; q.out_by=-1;
        S.P.push_back(q);
        S.last_hit.push_back({-1,0});
    }
    for(auto&b:obs.at("bombs").as_array()){
        Bomb bb; bb.x=b.at("x").as_double(); bb.y=b.at("y").as_double();
        bb.vx=b.at("vx").as_double(); bb.vy=b.at("vy").as_double();
        bb.fuse=int(b.at("fuse").as_double()+1e-9); bb.owner=int(b.at("owner").as_double()+1e-9); bb.id=int(b.at("id").as_double()+1e-9);
        S.B.push_back(bb);
    }
    
    S.next_bomb=0; for(auto&b:S.B) S.next_bomb=std::max(S.next_bomb,b.id+1);
    for(auto&w:obs.at("walls").as_array()){
        Wall ww; ww.x1=w.at("x1").as_double(); ww.y1=w.at("y1").as_double();
        ww.x2=w.at("x2").as_double(); ww.y2=w.at("y2").as_double();
        ww.spring=w.at("kind").as_string()=="spring";
        S.W.push_back(ww);
    }
    for(auto&w:obs.at("broken").as_array()){
        Wall ww; ww.x1=w.at("x1").as_double(); ww.y1=w.at("y1").as_double();
        ww.x2=w.at("x2").as_double(); ww.y2=w.at("y2").as_double(); ww.spring=false;
        S.brokenW.push_back(ww); S.brokenAt.push_back(int(w.at("at").as_double()+1e-9));
    }
    
    for(int i=0;i<S.n;++i){
        const J&h=obs.at("last_hit").at(i);
        if(!h.is_null()) S.last_hit[i]={int(h.at(0).as_double()+1e-9),int(h.at(1).as_double()+1e-9)};
    }
    return S;
}

// ---------- evaluation ----------
struct Eval { double score; };

static double evaluate(const State&S,int me){
    if(S.P[me].out) return NEG;
    double sc=0;
    // count falls
    for(int i=0;i<S.n;++i){
        if(i==me)continue;
        if(S.P[i].out && S.P[i].out_by==me) sc+=cfg.kill_bonus;
    }
    double half=S.half;
    double mx=S.P[me].x,my=S.P[me].y;
    double depth=half-std::max(std::abs(mx),std::abs(my));
    if(depth<cfg.edge_margin) sc+=cfg.near_edge*(cfg.edge_margin-depth)/cfg.edge_margin;
    // mild global center preference (rank1 sits at 0.36 of half; I was at 0.42)
    sc-=0.15*(std::max(std::abs(mx),std::abs(my))/half);
    // nearest enemy
    double nd=1e9; int ni=-1;
    for(int i=0;i<S.n;++i){if(i==me||S.P[i].out)continue;
        double d=hyp(S.P[i].x-mx,S.P[i].y-my); if(d<nd){nd=d;ni=i;}}
    if(ni>=0){
        sc+=cfg.proximity*nd*0.1;
        // threat: enemy holding a charged hammer/attack and I'm in their reach
        const auto&E=S.P[ni];
        double threat=0;
        if(E.skill==2&&E.charge>0.2&&nd<3.4) threat+=cfg.bomb_danger*E.charge*(3.4-nd)/3.4;
        if(E.attack_cd==0&&E.act==0&&nd<3.0) threat+=cfg.bomb_danger*0.4;
        if(E.skill==1&&E.charge>0.5&&nd<6.0) threat+=cfg.bomb_danger*0.3*E.charge;
        sc-=threat;
        // reward my own ready hammer at engage distance
        if(S.P[me].skill==2&&S.P[me].charge>0.5&&nd<3.2) sc+=1.0;
    }
    return sc;
}

static double simulate_and_score(const State&S0,const Order&mine,int me,int horizon,double enemy_aggr){
    State S=S0;
    std::vector<const Order*> orders(S.n,nullptr);
    Order enemies;  // shared simple enemy behaviour: walk to nearest foe
    for(int t=0;t<horizon;++t){
        // build orders fresh each turn
        std::vector<const Order*> os(S.n,nullptr);
        os[me]=&mine;
        // enemies: aggressive model that uses their actual held skills.
        std::vector<Order> eorders(S.n);
        for(int s=0;s<S.n;++s){
            if(s==me||S.P[s].out)continue;
            Order&o=eorders[s];
            double bx=0,by=0,bd=1e9; int bq=-1;
            for(int q=0;q<S.n;++q){if(q==s||S.P[q].out)continue;
                double d=hyp(S.P[q].x-S.P[s].x,S.P[q].y-S.P[s].y);
                if(d<bd){bd=d;bx=S.P[q].x-S.P[s].x;by=S.P[q].y-S.P[s].y;bq=q;}}
            if(bd>1e8){bx=-S.P[s].x;by=-S.P[s].y;bd=hyp(bx,by);bq=-1;}
            double ux=0,uy=0; if(bd>1e-9){ux=bx/bd;uy=by/bd;}
            auto&P=S.P[s];
            bool swung=false;
            if(P.act==0){
                if(P.skill==2&&P.charge>0.25&&bd<3.0&&bq>=0){ // hammer swing at nearest
                    o.has_hammer=true;o.hx=ux;o.hy=uy;swung=true;
                }else if(P.skill==1&&P.charge>0.5&&bd<7&&bq>=0){ // dash in
                    o.has_dash=true;o.dx=ux;o.dy=uy;swung=true;
                }
            }
            if(!swung){
                if(bd>1e-9){o.has_move=true;o.mx=ux;o.my=uy;}
                if(bd<2.2&&P.attack_cd==0&&P.act==0){o.has_attack=true;o.ax=ux;o.ay=uy;}
                // keep charging whatever they hold, or start hammer if idle
                if(P.skill!=0)o.charge=P.skill; else if(P.bar>0.5)o.charge=2;
            }
        }
        for(int s=0;s<S.n;++s) if(s!=me&&!S.P[s].out) os[s]=&eorders[s];
        sim::step_turn(S,os);
        if(S.alive_count()<=1)break;
    }
    (void)enemy_aggr;
    double base=evaluate(S,me);
    // reward enemies falling at any point: check out flags
    return base;
}

// positional score for a candidate move only (no sim)
static double heuristic(State&S,int me){
    return evaluate(S,me);
}

static Order empty_order(){Order o;return o;}

static double v_slip(double v,double fr){ // slide distance until stop under friction fr
    if(v<=0)return 0;
    double keep=1-fr,d=0;int guard=0;
    while(v>1e-4&&guard++<4000){v*=keep;d+=v;}
    return d;
}

// -------- top-level step --------
static J decide(const J& obs){
    if(obs.at("me").at("out").as_bool()) return J::object{};
    int me=int(obs.at("you").as_double()+1e-9);
    g_you=me;
    State S0=build_state(obs);
    const auto&Me=S0.P[me];
    int n_alive=S0.alive_count();

    // Pre-safety: if standing still gets me killed, prefer brake moves.
    // Candidate set: brake (opposite velocity), to-center, 8 dirs, stay.
    std::vector<std::pair<double,double>> cands;
    auto add_dir=[&](double x,double y){double h=hyp(x,y); if(h<1e-9)return; cands.push_back({x/h,y/h});};
    add_dir(-Me.vx,-Me.vy);
    add_dir(-Me.x,-Me.y);
    add_dir(Me.vx,Me.vy);
    for(int k=0;k<8;++k){double a=2*M_PI*k/8; add_dir(std::cos(a),std::sin(a));}
    add_dir(0,0);

    double best_score=NEG; int best_i=-1;
    Order best_order;
    // score each candidate by simulating me moving that way
    std::vector<Order> cand_orders;
    for(auto&c:cands){
        Order o; if(c.first!=0||c.second!=0){o.has_move=true;o.mx=c.first;o.my=c.second;}
        cand_orders.push_back(o);
    }
    // If I hold a hammer charge, also consider releasing toward nearest enemy.
    Order keep_charge;
    int tgt=-1; double td=1e9;
    for(int i=0;i<S0.n;++i){if(i==me||S0.P[i].out)continue;
        double d=hyp(S0.P[i].x-Me.x,S0.P[i].y-Me.y); if(d<td){td=d;tgt=i;}}
    bool hold_hammer=Me.skill==2;
    // Anti-overcharge / anti-passivity: a held charge stops regen and dies at 45 held ticks.
    // If I am holding with a usable amount and either (a) held is high, or (b) bar is empty
    // and no good target, prefer releasing or re-planning over passively holding.
    bool must_spend = Me.skill!=0 && Me.charge>0.15 &&
        (Me.held>=36 || (Me.bar<=0.01 && Me.held>=24));
    double vx=0,vy=0; if(tgt>=0){vx=S0.P[tgt].x-Me.x;vy=S0.P[tgt].y-Me.y;double h=hyp(vx,vy);if(h>1e-9){vx/=h;vy/=h;}}
    // variants: keep charging hammer; release hammer; keep charging dash; release dash
    std::vector<Order> extra;
    if(hold_hammer&&Me.charge>0.15){
        {Order o; o.has_hammer=true;o.hx=vx;o.hy=vy; extra.push_back(o);}
        {Order o; o.has_hammer=true;o.hx=vx;o.hy=vy;o.has_move=true;o.mx=-vx;o.my=-vy; extra.push_back(o);}
        if(must_spend&&tgt>=0){Order o; o.has_hammer=true;o.hx=vx;o.hy=vy;o.has_move=true;o.mx=vx;o.my=vy; extra.push_back(o);}
    }
    if(Me.skill==1&&Me.charge>0.5){
        {Order o; o.has_dash=true;o.dx=vx;o.dy=vy; extra.push_back(o);}
        {Order o; o.has_dash=true;o.dx=vx;o.dy=vy;o.has_move=true;o.mx=vx;o.my=vy; extra.push_back(o);}
    }
    // dash-chase: target shallow near the edge, I hold dash charge, and I can reach them
    if(tgt>=0&&Me.skill==1&&Me.charge>0.45){
        auto&E=S0.P[tgt];
        double dep=S0.half-std::max(std::abs(E.x),std::abs(E.y));
        double dd=hyp(E.x-Me.x,E.y-Me.y);
        if(dep<3.0&&dd<Me.charge*0.9*10+2){
            double ax=E.x-Me.x,ay=E.y-Me.y,h=hyp(ax,ay);
            if(h>1e-9){
                Order o; o.has_dash=true;o.dx=ax/h;o.dy=ay/h; extra.push_back(o);
                Order o2=o; o2.has_move=true;o2.mx=ax/h;o2.my=ay/h; extra.push_back(o2);
            }
        }
    }
    // charge dash when the nearest enemy is at mid distance and I have full bar
    if(Me.skill==0&&Me.bar>0.95&&td>5.0){
        Order o; o.charge=1; extra.push_back(o);
    }
    // outnumbered flee: >=2 enemies within 4.5 and I hold dash -> burst away from their centroid
    {
        int near=0; double cx=0,cy=0;
        for(int i=0;i<S0.n;++i){if(i==me||S0.P[i].out)continue;
            double dd=hyp(S0.P[i].x-Me.x,S0.P[i].y-Me.y);
            if(dd<4.5){++near;cx+=S0.P[i].x-Me.x;cy+=S0.P[i].y-Me.y;}}
        if(near>=2&&Me.skill==1&&Me.charge>0.4){
            double h=hyp(cx,cy);
            if(h>1e-9){Order o; o.has_dash=true;o.dx=-cx/h;o.dy=-cy/h; extra.push_back(o);}
        }
    }
    // emergency dash: if sliding toward the edge and I hold dash charge, brake/release inward
    {
        double nx=Me.x,ny=Me.y,h=std::max(std::abs(nx),std::abs(ny));
        double ax=(h>1e-9)?nx/h:0, ay=(h>1e-9)?ny/h:0;      // outward unit
        double outward=Me.vx*ax+Me.vy*ay;
        double margin=S0.half-h-0.6;
        bool danger= margin<1.2 && outward>0.05;
        if(danger&&Me.skill==1&&Me.charge>0.2){
            Order o; o.has_dash=true; o.dx=-ax; o.dy=-ay; extra.push_back(o);
        }
    }
    // start charging hammer/dash when idle and not in immediate danger
    if(td>4.5||Me.skill!=0){
        {Order o; o.charge=2; extra.push_back(o);}
        {Order o; o.charge=1; extra.push_back(o);}
    }
    // throw bomb toward target edge-side
    if(Me.bomb_cd==0&&tgt>=0){
        // lead the target: bomb keeps ~0.5*(0.95^k) per tick; aim where they will be
        double tx=S0.P[tgt].x+S0.P[tgt].vx*20, ty=S0.P[tgt].y+S0.P[tgt].vy*20;
        double bdx=tx-Me.x,bdy=ty-Me.y,bd=hyp(bdx,bdy);
        if(bd>1e-9){bdx/=bd;bdy/=bd;}else{bdx=vx;bdy=vy;}
        int fuse=clampd(bd/(0.5*0.93),24,90);
        Order o; o.has_bomb=true;o.bx=bdx;o.by=bdy;o.fuse=(int)fuse; extra.push_back(o);
        Order o2=o; o2.fuse=clampd(bd/(0.5*0.93)+6,24,90); extra.push_back(o2);
        // defensive lob just outside melee range
        if(td<4.5){Order o3; o3.has_bomb=true;o3.bx=vx;o3.by=vy;o3.fuse=30; extra.push_back(o3);}
        // rank1-style mid-range pressure: short-fuse bomb at the nearest enemy 3.5-7 away
        if(td>=3.5&&td<7.0){
            double tx=S0.P[tgt].x+S0.P[tgt].vx*14, ty=S0.P[tgt].y+S0.P[tgt].vy*14;
            double bdx=tx-Me.x,bdy=ty-Me.y,bd=hyp(bdx,bdy);
            if(bd>1e-9){bdx/=bd;bdy/=bd;}
            Order o4; o4.has_bomb=true;o4.bx=bdx;o4.by=bdy;o4.fuse=24; extra.push_back(o4);
            Order o5=o4; o5.fuse=36; extra.push_back(o5);
        }
    }
    // jab if in range (slightly generous: windup gives closing time)
    if(Me.attack_cd==0&&Me.act==0&&td<3.1){
        Order o; o.has_attack=true;o.ax=vx;o.ay=vy; extra.push_back(o);
        {Order o2=o; o2.has_move=true; o2.mx=-vx; o2.my=-vy; extra.push_back(o2);}   // jab & back off
        {Order o3=o; o3.has_move=true; o3.mx=vx;  o3.my=vy;  extra.push_back(o3);}   // jab & push in
    }
    // kite: charge hammer while moving away from the nearest enemy
    if(Me.skill!=2&&td<7&&td>2.5){
        Order o; o.charge=2; o.has_move=true; o.mx=-vx; o.my=-vy; extra.push_back(o);
    }
    // orbit: when the nearest enemy holds a charged hammer and I am inside its reach+buffer,
    // circle-strafe at a safe radius instead of standing in the cone.
    if(tgt>=0&&td<4.2){
        const auto&E=S0.P[tgt];
        bool hammer_threat=E.skill==2&&E.charge>0.25;
        bool jab_threat=E.attack_cd==0&&E.act==0&&td<3.0;
        if(hammer_threat||jab_threat){
            double px=-vy, py=vx;                 // perpendicular to enemy->me
            double pl=hyp(px,py); if(pl>1e-9){px/=pl;py/=pl;
                Order o; o.has_move=true; o.mx=px; o.my=py; extra.push_back(o);
                Order o2; o2.has_move=true; o2.mx=-px; o2.my=-py; extra.push_back(o2);
                // back out of reach entirely
                Order o3; o3.has_move=true; o3.mx=-vx; o3.my=-vy; extra.push_back(o3);
            }
        }
    }

    if(best_i<0){                    // no candidate scored (all doomed or budget cut): brake toward centre
        Order o; o.has_move=true;
        double bx=-Me.x,by=-Me.y,h=hyp(bx,by);
        if(h<1e-9){bx=-Me.vx;by=-Me.vy;h=hyp(bx,by);}
        if(h>1e-9){o.mx=bx/h;o.my=by/h;}
        else{o.mx=0;o.my=1;}
        best_order=o;
    }
    // ---- shrink-aware safety override: add strong inward candidates when near the safe edge ----
    {
        double half_min=S0.half;
        for(size_t k=0;k<S0.sh_turn.size();++k){
            int start=(S0.sh_turn[k]-1)*S0.D, end=S0.sh_end_turn[k]*S0.D, t_now=S0.turn*S0.D;
            if((t_now>=start&&t_now<=end)||(t_now<start&&start-t_now<=S0.D*10))
                half_min=std::min(half_min,S0.sh_to[k]);
        }
        double m=std::max(std::abs(Me.x),std::abs(Me.y));
        double outv=(m>1e-9)?(Me.vx*(Me.x/m)+Me.vy*(Me.y/m)):0.0;
        double drift=v_slip(outv>0?outv:0.0,S0.R.friction);
        double danger_at=half_min-1.0;
        if(m+drift>danger_at){
            Order o; o.has_move=true;
            double cx=-Me.x, cy=-Me.y, ch=hyp(cx,cy);
            if(ch<1e-9){cx=-Me.vx;cy=-Me.vy;ch=hyp(cx,cy);}
            if(ch>1e-9){o.mx=cx/ch;o.my=cy/ch;}else{o.mx=0;o.my=1;}
            if(std::abs(Me.x)>std::abs(Me.y))o.my*=0.35; else o.mx*=0.35;   // corner: cut the bigger axis
            double nn=hyp(o.mx,o.my); o.mx/=nn; o.my/=nn;
            extra.push_back(o);
            cand_orders.push_back(o);
        }
    }
    double t_start=(double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int budget_usecs=50000;

    // Score move candidates (sim)
    for(size_t ci=0;ci<cand_orders.size();++ci){
        double now=(double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(now-t_start>budget_usecs*0.5) break;
        double sc=simulate_and_score(S0,cand_orders[ci],me,cfg.horizon,1.0)+1e-6*ci;
        if(sc>best_score){best_score=sc;best_i=(int)ci;best_order=cand_orders[ci];}
    }
    // Score extras (charge/release/bomb/jab combined with a sensible move)
    for(auto&ex:extra){
        double now=(double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(now-t_start>budget_usecs) break;
        // give it the best-so-far move or toward/away target
        Order o=ex;
        if(!o.has_move&&best_i>=0&&!cands[best_i].first==false){}
        double sc=simulate_and_score(S0,o,me,cfg.horizon,1.0);
        if(sc>best_score){best_score=sc;best_order=o;}
    }

    // assemble JSON order
    J out=J::object{};
    const Order&o=best_order;
    if(o.has_move) out["move"]=J::array{o.mx,o.my};
    if(o.charge==1) out["charge"]="dash";
    if(o.charge==2) out["charge"]="hammer";
    if(o.has_dash) out["dash"]=J::array{o.dx,o.dy};
    if(o.has_hammer) out["hammer"]=J::array{o.hx,o.hy};
    if(o.has_bomb) out["bomb"]=J{{"dir",J::array{o.bx,o.by}},{"fuse",o.fuse}};
    if(o.has_attack) out["attack"]=J::array{o.ax,o.ay};
    return out;
}

int main(){
    return kb_step_worker::run([](const J& obs, J& memory){
        J order=J::object{};
        try{ order=decide(obs); }
        catch(const std::exception&e){ std::cerr<<"DECIDE-ERR: "<<e.what()<<"\n"; order=J::object{}; }
        catch(...){ std::cerr<<"DECIDE-ERR: unknown\n"; order=J::object{}; }
        return std::make_pair(order,memory);
    });
}
