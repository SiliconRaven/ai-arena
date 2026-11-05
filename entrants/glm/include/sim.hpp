// Lightweight bit-exact roll-forward of the judge engine (native/include/engine.hpp).
// Only what the policy needs: move, charge, dash, hammer, jab, bombs, walls, friction,
// rounding, falls. Expression order stays aligned with engine.hpp (same f/p ops).
#pragma once
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include "setup.hpp"
#include "physics.hpp"

namespace sim {
using knockback::Body;
using knockback::pair;
using knockback::py_round;
using knockback::unit;

struct Wall { double x1,y1,x2,y2; bool spring; };
struct Bomb { double x,y,vx,vy; int fuse,owner,id; };
struct Player {
    double x,y,vx,vy,bar,charge;
    int skill;                 // 0 none, 1 dash, 2 hammer
    int pause,held,act;        // act 0 none, 1 recover, 2 jab windup, 3 jab_recover
    int act_ticks,attack_cd,hammer_cd,bomb_cd;
    double aimx,aimy;
    bool out; int out_tick,out_by;   // out_by -1 none/self
};
struct Rules {
    double player_radius,bomb_radius,bomb_mass,bomb_friction,bomb_throw_speed,bomb_spawn_gap;
    double friction,restitution,min_speed,min_impulse;
    double move_accel,dash_charging_factor,hammer_charging_factor;
    double bar_charge_rate,bar_refill_rate; int bar_pause_ticks,bar_overcharge_ticks;
    double dash_speed;
    double hammer_reach,hammer_cone_cos,hammer_power,hammer_recoil; int hammer_recover_ticks,hammer_cooldown_ticks;
    double attack_reach,attack_cone_cos,attack_power,attack_recoil; int attack_windup_ticks,attack_recover_ticks,attack_cooldown_ticks;
    int bomb_fuse_min,bomb_fuse_max,bomb_cooldown_ticks; double bomb_blast_radius,bomb_blast_power;
    double walls_thickness,walls_restitution,walls_regen_push; int walls_regen_ticks,walls_warn_ticks;
    double spring_factor,spring_max_speed;
    int decision_ticks,max_turns,precision,kill_credit_ticks;
    double arena_half;
};
struct Order {
    bool has_move=false; double mx=0,my=0;
    int charge=0;
    bool has_dash=false; double dx=0,dy=0;
    bool has_hammer=false; double hx=0,hy=0;
    bool has_bomb=false; double bx=0,by=0; int fuse=0;
    bool has_attack=false; double ax=0,ay=0;
};
inline std::pair<double,double> norm_dir(double x,double y){
    double big=std::max(std::abs(x),std::abs(y));
    if(big>1e150){x/=big;y/=big;}
    auto u=unit(x,y);
    return u? *u : std::make_pair(0.0,0.0);
}
struct State {
    Rules R;
    std::vector<Player> P;
    std::vector<Bomb> B;
    std::vector<Wall> W;                 // standing, referee order
    std::vector<Wall> brokenW;           // broken walls (normal), referee order
    std::vector<int> brokenAt;
    std::vector<std::pair<int,int>> last_hit;   // seat,tick  (-1,0 if none)
    int turn=0,tick=0,D=6,n=0,next_bomb=0;
    double half=0;
    std::vector<int> sh_turn,sh_end_turn; std::vector<double> sh_from,sh_to;
    std::vector<int> alive_order() const { std::vector<int> a; for(int i=0;i<n;++i) if(!P[i].out) a.push_back(i); return a; }
    int alive_count() const { int c=0; for(auto&p:P) if(!p.out) ++c; return c; }
};

inline void apply_shrink(State&S,int t){
    for(size_t k=0;k<S.sh_turn.size();++k){
        int start=(S.sh_turn[k]-1)*S.D+1,end=S.sh_end_turn[k]*S.D;
        if(start<=t&&t<=end) S.half=py_round(S.sh_from[k]+(S.sh_to[k]-S.sh_from[k])*(t-start+1.0)/(end-start+1.0),4);
    }
}
inline bool wall_off(const Wall&w,double half){return std::max({std::abs(w.x1),std::abs(w.y1),std::abs(w.x2),std::abs(w.y2)})>half;}
inline std::pair<double,double> wall_nearest(double px,double py,const Wall&w){
    double ex=w.x2-w.x1,ey=w.y2-w.y1,l=ex*ex+ey*ey;
    double t=l==0?0:std::max(0.,std::min(1.,((px-w.x1)*ex+(py-w.y1)*ey)/l));
    return {w.x1+t*ex,w.y1+t*ey};
}
inline double wall_dist(double px,double py,const Wall&w){
    auto q=wall_nearest(px,py,w); double dx=px-q.first,dy=py-q.second; return std::sqrt(dx*dx+dy*dy);
}
inline void fall_out(State&S,int s,int tick){
    auto&p=S.P[s]; p.out=true; p.vx=0; p.vy=0; p.act=0; p.act_ticks=0;
    auto&h=S.last_hit[s];
    p.out_by=(h.first>=0&&h.first!=s&&tick-h.second<=S.R.kill_credit_ticks)?h.first:-1;
}
inline void throw_bomb(State&S,int seat,double dirx,double diry,int fuse,int tick){
    auto&p=S.P[seat]; auto&R=S.R;
    double gap=R.player_radius+R.bomb_radius+R.bomb_spawn_gap;
    Bomb b; b.x=p.x+dirx*gap; b.y=p.y+diry*gap;
    b.vx=p.vx+dirx*R.bomb_throw_speed; b.vy=p.vy+diry*R.bomb_throw_speed;
    b.fuse=fuse; b.owner=seat; b.id=S.next_bomb++;
    S.B.push_back(b); p.bomb_cd=R.bomb_cooldown_ticks;
}
inline void swing(State&S,int seat,int tick,bool hammer,double ax,double ay,double scale){
    auto&p=S.P[seat]; auto&R=S.R;
    double reach=hammer?R.hammer_reach:R.attack_reach;
    double gain=hammer?R.hammer_power:R.attack_power;
    double cone=hammer?R.hammer_cone_cos:R.attack_cone_cos;
    for(int s=0;s<S.n;++s){
        auto&q=S.P[s];
        if(s==seat||q.out)continue;
        double dx=q.x-p.x,dy=q.y-p.y;
        if(dx*dx+dy*dy>reach*reach)continue;
        auto u=unit(dx,dy).value_or(std::make_pair(ax,ay));
        if(u.first*ax+u.second*ay<cone)continue;
        q.vx+=u.first*gain*scale; q.vy+=u.second*gain*scale;
        S.last_hit[s]={seat,tick};
    }
    for(auto&b:S.B){
        double dx=b.x-p.x,dy=b.y-p.y;
        if(dx*dx+dy*dy>reach*reach)continue;
        auto u=unit(dx,dy).value_or(std::make_pair(ax,ay));
        if(u.first*ax+u.second*ay<cone)continue;
        b.vx+=u.first*gain*scale/R.bomb_mass; b.vy+=u.second*gain*scale/R.bomb_mass;
        b.owner=seat;
    }
    double rec=hammer?R.hammer_recoil:R.attack_recoil;
    p.vx-=ax*rec*scale; p.vy-=ay*rec*scale;
}
inline void explode(State&S,const Bomb&bomb,int tick){
    auto&R=S.R; double reach=R.bomb_blast_radius;
    auto push=[&](double&x,double&y,double&vx,double&vy,double mass){
        double dx=x-bomb.x,dy=y-bomb.y,d2=dx*dx+dy*dy;
        if(d2>reach*reach)return false;
        auto u=unit(dx,dy).value_or(std::make_pair(1.,0.));
        double gain=R.bomb_blast_power*(1-std::sqrt(d2)/reach)/mass;
        vx+=u.first*gain; vy+=u.second*gain;
        return true;
    };
    for(int s=0;s<S.n;++s){auto&p=S.P[s];
        if(!p.out&&push(p.x,p.y,p.vx,p.vy,1.)) S.last_hit[s]={bomb.owner,tick};
    }
    for(auto&b:S.B) push(b.x,b.y,b.vx,b.vy,R.bomb_mass);
    // break normal walls
    for(size_t k=0;k<S.W.size();){
        auto w=S.W[k];
        if(w.spring){++k;continue;}
        auto q=wall_nearest(bomb.x,bomb.y,w);
        double dx=q.first-bomb.x,dy=q.second-bomb.y;
        if(dx*dx+dy*dy<=reach*reach){
            S.brokenW.push_back(w); S.brokenAt.push_back(tick+R.walls_regen_ticks);
            S.W.erase(S.W.begin()+k);
        }else ++k;
    }
}
inline void bounce_wall(State&S,double&x,double&y,double&vx,double&vy,double radius,const Wall&w){
    double reach=radius+S.R.walls_thickness/2;
    auto q=wall_nearest(x,y,w);
    double dx=x-q.first,dy=y-q.second,d2=dx*dx+dy*dy;
    if(d2>=reach*reach)return;
    double nx,ny;
    if(d2>0){double d=std::sqrt(d2);nx=dx/d;ny=dy/d;}
    else{auto u=unit(-(w.y2-w.y1),w.x2-w.x1).value_or(std::make_pair(1.,0.));nx=u.first;ny=u.second;}
    x=q.first+nx*reach; y=q.second+ny*reach;
    double inward=vx*nx+vy*ny;
    if(inward>=0)return;
    double speed=w.spring?std::min(S.R.spring_factor*-inward,S.R.spring_max_speed):S.R.walls_restitution*-inward;
    vx+=(speed-inward)*nx; vy+=(speed-inward)*ny;
}
inline void regrow_wall(State&S,const Wall&w,int tick){
    auto push=[&](double&x,double&y,double&vx,double&vy,double radius){
        double reach=radius+S.R.walls_thickness/2;
        auto q=wall_nearest(x,y,w);
        double dx=x-q.first,dy=y-q.second;
        if(dx*dx+dy*dy>=reach*reach)return false;
        double nx,ny;
        double d2=dx*dx+dy*dy;
        if(d2>0){double d=std::sqrt(d2);nx=dx/d;ny=dy/d;}
        else{auto u=unit(-(w.y2-w.y1),w.x2-w.x1).value_or(std::make_pair(1.,0.));nx=u.first;ny=u.second;}
        x=q.first+nx*reach; y=q.second+ny*reach;
        double normal=vx*nx+vy*ny;
        vx+=(S.R.walls_regen_push-normal)*nx; vy+=(S.R.walls_regen_push-normal)*ny;
        return true;
    };
    for(auto&p:S.P) if(!p.out) push(p.x,p.y,p.vx,p.vy,S.R.player_radius);
    for(auto&b:S.B) push(b.x,b.y,b.vx,b.vy,S.R.bomb_radius);
}
// one physics tick. orders indexed by seat, nullptr for "no decision" seats.
inline void tick(State&S,const std::vector<const Order*>&orders,int t){
    apply_shrink(S,t);
    {std::vector<Wall> keep; for(auto&w:S.W) if(!wall_off(w,S.half)) keep.push_back(w); S.W=std::move(keep);}
    {std::vector<Wall> wb; std::vector<int> ab;
     for(size_t k=0;k<S.brokenW.size();){
        const Wall&w=S.brokenW[k];
        if(wall_off(w,S.half)){S.brokenW.erase(S.brokenW.begin()+k);S.brokenAt.erase(S.brokenAt.begin()+k);}
        else if(t>=S.brokenAt[k]){regrow_wall(S,w,t);
            S.W.push_back(w); std::sort(S.W.begin(),S.W.end(),[](const Wall&a,const Wall&b){return false;});
            S.brokenW.erase(S.brokenW.begin()+k);S.brokenAt.erase(S.brokenAt.begin()+k);}
        else ++k;
     }}
    for(size_t k=0;k<S.B.size();){
        auto&b=S.B[k];
        b.fuse-=1;
        if(b.fuse<=0){Bomb dead=b; S.B.erase(S.B.begin()+k); explode(S,dead,t);}
        else ++k;
    }
    auto a=S.alive_order();
    bool first=((t-1)%S.D)==0;
    for(int s:a){
        auto&p=S.P[s]; auto&R=S.R;
        const Order* o=(size_t(s)<orders.size())?orders[s]:nullptr;
        Order none;
        const Order& oo=o?*o:none;
        p.attack_cd=std::max(0,p.attack_cd-1); p.hammer_cd=std::max(0,p.hammer_cd-1);
        bool busy=p.act!=0;
        if(busy){
            p.act_ticks-=1;
            if(p.act_ticks<=0){
                if(p.act==2){swing(S,s,t,false,p.aimx,p.aimy,1.); p.act=3; p.act_ticks=R.attack_recover_ticks; p.aimx=p.aimy=0;}
                if((p.act==1||p.act==3)&&p.act_ticks<=0){p.act=0;p.act_ticks=0;}
            }
        }
        int want=oo.charge;
        if(want==2&&p.hammer_cd>0)want=0;
        bool release=!busy&&first&&p.skill!=0&&p.charge>0&&((p.skill==1&&oo.has_dash)||(p.skill==2&&oo.has_hammer));
        if(!busy&&!release&&want!=0&&p.skill!=0&&p.skill!=want){p.charge=0;p.skill=0;}
        bool jab=first&&!busy&&oo.has_attack&&p.attack_cd==0&&!(release&&p.skill==2);
        bool charging=!busy&&!release&&!jab&&want!=0&&p.bar>0;
        if(p.pause>0)p.pause-=1;
        else if(!charging)p.bar=std::max(p.bar,std::min(1.-p.charge,p.bar+R.bar_refill_rate));
        if(release){
            int sk=p.skill;               // capture before clearing (referee reads it first)
            double power=p.charge; p.charge=0; p.skill=0; p.pause=R.bar_pause_ticks;
            if(sk==1){p.vx+=oo.dx*R.dash_speed*power; p.vy+=oo.dy*R.dash_speed*power;}
            else{p.hammer_cd=R.hammer_cooldown_ticks; swing(S,s,t,true,oo.hx,oo.hy,power); p.act=1; p.act_ticks=R.hammer_recover_ticks;}
        }
        if(jab){
            auto d=norm_dir(oo.ax,oo.ay);
            p.act=2; p.act_ticks=R.attack_windup_ticks; p.aimx=d.first; p.aimy=d.second;
            p.attack_cd=R.attack_cooldown_ticks;
        }
        if(charging){
            double amount=std::min(R.bar_charge_rate,p.bar);
            p.bar-=amount; p.charge+=amount; p.skill=want;
        }
        if(p.charge<=0||charging)p.held=0;
        else{p.held+=1;
            if(p.held>=R.bar_overcharge_ticks){p.charge=0;p.skill=0;p.held=0;p.pause=R.bar_pause_ticks;}
        }
        if(oo.has_move&&!busy&&p.act==0){
            double factor=p.skill==1?R.dash_charging_factor:p.skill==2?R.hammer_charging_factor:1.;
            p.vx+=oo.mx*R.move_accel*factor; p.vy+=oo.my*R.move_accel*factor;
        }
        if(p.bomb_cd>0)p.bomb_cd-=1;
        else if(first&&oo.has_bomb&&!busy&&p.act==0) throw_bomb(S,s,oo.bx,oo.by,oo.fuse,t);
    }
    for(int s:a){auto&p=S.P[s]; p.x+=p.vx; p.y+=p.vy;}
    for(auto&b:S.B){b.x+=b.vx;b.y+=b.vy;}
    // collisions: player-player
    auto&R=S.R;
    for(size_t k=0;k<a.size();++k)for(size_t l=k+1;l<a.size();++l){
        int i=a[k],j=a[l];
        Body A{S.P[i].x,S.P[i].y,S.P[i].vx,S.P[i].vy},Bb{S.P[j].x,S.P[j].y,S.P[j].vx,S.P[j].vy};
        double imp=pair(A,Bb,R.player_radius*2,1.,1.,R.restitution);
        S.P[i].x=A.x;S.P[i].y=A.y;S.P[i].vx=A.vx;S.P[i].vy=A.vy;
        S.P[j].x=Bb.x;S.P[j].y=Bb.y;S.P[j].vx=Bb.vx;S.P[j].vy=Bb.vy;
        if(imp>=R.min_impulse){S.last_hit[i]={j,t};S.last_hit[j]={i,t};}
    }
    for(int i:a){auto&p=S.P[i];
        for(auto&b:S.B){
            double nx=b.x-p.x,ny=b.y-p.y;
            bool hitter=(p.vx+b.vx)*nx+(p.vy+b.vy)*ny>0;
            Body A{p.x,p.y,p.vx,p.vy},Bb{b.x,b.y,b.vx,b.vy};
            double imp=pair(A,Bb,R.player_radius+R.bomb_radius,1.,R.bomb_mass,R.restitution);
            p.x=A.x;p.y=A.y;p.vx=A.vx;p.vy=A.vy; b.x=Bb.x;b.y=Bb.y;b.vx=Bb.vx;b.vy=Bb.vy;
            if(imp>=R.min_impulse&&hitter)b.owner=i;
        }
    }
    for(size_t k=0;k<S.B.size();++k)for(size_t l=k+1;l<S.B.size();++l){
        Body A{S.B[k].x,S.B[k].y,S.B[k].vx,S.B[k].vy},Bb{S.B[l].x,S.B[l].y,S.B[l].vx,S.B[l].vy};
        double imp=pair(A,Bb,R.bomb_radius*2,R.bomb_mass,R.bomb_mass,R.restitution);
        S.B[k].x=A.x;S.B[k].y=A.y;S.B[k].vx=A.vx;S.B[k].vy=A.vy;
        S.B[l].x=Bb.x;S.B[l].y=Bb.y;S.B[l].vx=Bb.vx;S.B[l].vy=Bb.vy;
    }
    for(int i:a) for(auto&w:S.W) bounce_wall(S,S.P[i].x,S.P[i].y,S.P[i].vx,S.P[i].vy,R.player_radius,w);
    for(auto&b:S.B) for(auto&w:S.W) bounce_wall(S,b.x,b.y,b.vx,b.vy,R.bomb_radius,w);
    // friction + rounding
    auto friction=[&](double&x,double&y,double&vx,double&vy,double fr){
        double keep=1-fr,ovx=vx,ovy=vy;
        vx*=keep; vy*=keep;
        if(vx*vx+vy*vy<R.min_speed*R.min_speed){vx=0.;vy=0.;}
        x=py_round(x,4)+0.; y=py_round(y,4)+0.;
        vx=py_round(vx,4)+0.; vy=py_round(vy,4)+0.;
        if(keep<1&&vx==ovx)vx=0.;
        if(keep<1&&vy==ovy)vy=0.;
    };
    for(int s:a){auto&p=S.P[s];
        double bar=p.bar,charge=p.charge;
        friction(p.x,p.y,p.vx,p.vy,R.friction);
        p.bar=py_round(bar,4)+0.; p.charge=py_round(charge,4)+0.;
    }
    for(auto&b:S.B){
        double fuse=b.fuse;
        friction(b.x,b.y,b.vx,b.vy,R.bomb_friction);
        (void)fuse;
    }
    for(int s:a){auto&p=S.P[s];
        if(std::max(std::abs(p.x),std::abs(p.y))>S.half) fall_out(S,s,t);
    }
    for(size_t k=0;k<S.B.size();){
        auto&b=S.B[k];
        if(std::max(std::abs(b.x),std::abs(b.y))>S.half) S.B.erase(S.B.begin()+k);
        else ++k;
    }
    S.tick=t;
}
inline void step_turn(State&S,const std::vector<const Order*>&orders){
    int start=S.turn*S.D;
    for(int k=0;k<S.D;++k){
        tick(S,orders,start+k+1);
        if(S.alive_count()<=1)break;
    }
    ++S.turn;
}
} // namespace sim
