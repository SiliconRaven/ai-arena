#pragma once
#include "json.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <string>
namespace fast {
using J=mini_json::Json;
enum Skill{NONE,ATTACK,CHARGE,GUARD,BLINK,UPPER,SLAM,WAVE,GRAB,COUNTER,HOOK};
enum Phase{IDLE,CHARGING,WINDUP,ACTIVE,RECOVER};
inline const char* names[]={"","attack","charge","guard","blink","uppercut","slam","wave","grab","counter","hook"};
inline int skill(const J&j){if(!j.is_string())return 0;for(int i=1;i<=10;i++)if(j.as_string()==names[i])return i;return 0;}
inline double sq(double x){return x*x;} inline double clamp(double x,double a,double b){return std::max(a,std::min(b,x));}
inline double sign(double x){return x<0?-1.:1.;}
inline void unit(double &x,double &y,double fx=1,double fy=0){double l=std::sqrt(x*x+y*y);if(l<1e-9){x=fx;y=fy;}else{x/=l;y/=l;}}
struct Move{double cost=0,damage=0,power=0,offset=0,radius=0,lift=0;int windup=0,active=0,recover=0;};
struct Config{
 Move m[11]; double gravity=.035,run=.32,gacc=.08,aacc=.03,gfric=.75,afric=.985,hfric=.97,bounce=.6,maxfall=.8,half=.6,height=1.8,jump=.62,airjump=.55,refill=.004;
 double base=.3,scale=1.1,stunper=28,blastx=26,blasttop=24,blastbottom=-16,shrink=.02,topratio=.9,min_top=1,min_x=0,charge_short=1,charge_full=.5;
 int slam_cap=-1;
 int gap=4,respawn=60,invuln=90,credit=300,stunbase=6,stunmax=45,shrinkstart=3000;
 double x1=-13,x2=13,top=0,bottom=-4; std::array<std::array<double,3>,8> platforms{};int np=0;
 void load(const J&r){auto get=[](const J&j,const char*k,double d){return j.contains(k)?j.at(k).as_double():d;};
 for(int s=1;s<=10;s++){auto &v=r.at("moves").at(names[s]);auto&a=m[s];a.cost=get(v,"cost",0);a.damage=get(v,"damage",0);a.power=get(v,"power",0);a.offset=get(v,"offset",0);a.radius=get(v,"radius",0);a.lift=get(v,"lift",0);a.windup=get(v,"windup",0);a.active=get(v,"active",0);a.recover=get(v,"recover",1);}
 const auto&p=r.at("physics");gravity=get(p,"gravity",gravity);run=get(p,"run_speed",run);gacc=get(p,"ground_accel",gacc);aacc=get(p,"air_accel",aacc);gfric=get(p,"ground_friction",gfric);afric=get(p,"air_friction",afric);hfric=get(p,"hit_friction",hfric);bounce=get(p,"bounce",bounce);maxfall=get(p,"max_fall",maxfall);
 const auto&b=r.at("body");half=get(b,"half_width",half);height=get(b,"height",height);
 const auto&st=r.at("stage");const auto&sol=st.at("solids").at(0);x1=get(sol,"x1",x1);x2=get(sol,"x2",x2);top=get(sol,"top",top);bottom=get(sol,"bottom",bottom);np=0;for(auto &pl:st.at("platforms").as_array()){if(np<8)platforms[np++]={get(pl,"x1",0),get(pl,"x2",0),get(pl,"y",0)};}
 const auto&bl=st.at("blast");blastx=get(bl,"x",blastx);blasttop=get(bl,"top",blasttop);blastbottom=get(bl,"bottom",blastbottom);
 if(st.contains("blast_shrink")){auto&sh=st.at("blast_shrink");shrinkstart=get(sh,"start_tick",shrinkstart);shrink=get(sh,"rate",shrink);topratio=get(sh,"top_ratio",topratio);min_top=get(sh,"min_top",1);min_x=get(sh,"min_x",0);}
 charge_short=get(r.at("moves").at("charge"),"short_power_mult",1);charge_full=get(r.at("moves").at("charge"),"short_power_full_fraction",.5);slam_cap=int(get(r.at("moves").at("slam"),"landing_hitstun_cap",-1));
 jump=get(r.at("jump"),"ground_speed",jump);airjump=get(r.at("jump"),"air_speed",airjump);refill=get(r.at("meter"),"refill",refill);auto&kb=r.at("knockback");base=get(kb,"base",base);scale=get(kb,"scale",scale);stunper=get(kb,"hitstun_per_speed",stunper);stunbase=get(kb,"hitstun_base",stunbase);stunmax=get(kb,"hitstun_max",stunmax);gap=get(r,"gap_ticks",gap);respawn=get(r,"respawn_ticks",respawn);invuln=get(r,"respawn_invuln_ticks",invuln);credit=get(r,"kill_credit_ticks",credit);
 }
};
struct Order{double move=0,dx=1,dy=0;int skill=0;bool jump=false,drop=false;};
inline Order order(int sk=0,double dx=1,double dy=0,double move=0,bool jump=false,bool drop=false){Order o;o.skill=sk;o.dx=dx;o.dy=dy;unit(o.dx,o.dy);o.move=move;o.jump=jump;o.drop=drop;return o;}
inline J json(const Order&o){J j{{"move",o.move},{"jump",o.jump},{"drop",o.drop},{"dir",J::array{o.dx,o.dy}}};if(o.skill)j["skill"]=names[o.skill];return j;}
struct F{
 double x=0,y=0,vx=0,vy=0,damage=0,meter=1,dx=1,dy=0,power=1,hx=0,hy=0;
 int face=1,air=1,stocks=3,respawn=0,invuln=0,stun=0,gap=0,cd=0,drop=0,phase=0,pt=0,charge=0,mask=0,last=-1,lasttick=-9999,pulled=-1,pull=0,act=0;
 bool ground=true,out=false,reel=false,slamLand=false;
};
struct Shot{int id=0,owner=0,ttl=0,kind=0;double x=0,y=0,vx=0,vy=0,damage=0,power=0;bool dead=false;};
struct Hit{int a,t,s;double dx,dy,damage,power;};
struct State{
 std::array<F,4> f;std::array<Shot,32> shots;int ns=0,n=4,tick=0,next=1;std::array<int,4>kills{};std::array<double,4>dealt{},received{};std::array<int,4>tags{};
 static bool untouch(const F&f){return f.out||f.respawn||f.invuln||(f.act==BLINK&&f.phase==ACTIVE);}
 static bool guard(const F&f){return f.act==GUARD&&f.phase==ACTIVE;}
 static bool counter(const F&f){return f.act==COUNTER&&f.phase==ACTIVE;}
 void end(F&f,int gap){f.act=0;f.phase=0;f.pt=0;f.charge=0;f.mask=0;f.reel=false;f.gap=gap;}
 void cancel(int i){F&a=f[i];for(int k=0;k<ns;k++)if(shots[k].owner==i&&shots[k].kind==HOOK)shots[k].dead=true;end(a,a.gap);}
 void recover(int i,const Config&c){F&a=f[i];if(a.act==BLINK){a.vx*=.2;a.vy*=.2;}a.reel=false;a.phase=RECOVER;a.pt=std::max(1,c.m[a.act].recover);}
 void active(int i,const Config&c){F&a=f[i];a.phase=ACTIVE;a.mask=0;a.pt=c.m[a.act].active;
 if(a.act==UPPER){a.vy=.75;a.ground=false;a.dx=0;a.dy=1;}
 else if(a.act==SLAM){a.vy=-.95;a.vx*=.3;a.dx=0;a.dy=-1;}
 else if(a.act==BLINK){a.vx=a.dx*1.2;a.vy=a.dy*1.2;a.ground=false;}
 else if(a.act==WAVE||a.act==HOOK){if(ns<32){Shot b;b.id=next++;b.owner=i;b.kind=a.act;double speed=a.act==WAVE?.75:.7;b.vx=a.dx*speed;b.vy=a.dy*speed;b.ttl=a.act==WAVE?40:28;b.x=a.x+a.dx*(c.half+.5);b.y=a.y+c.height/2+a.dy*(c.half+.5);b.damage=c.m[a.act].damage;b.power=c.m[a.act].power;shots[ns++]=b;}if(a.act==WAVE){a.phase=RECOVER;a.pt=c.m[WAVE].recover;}else a.pt=1000000;}}
 void setdir(F&a,const Order&o){a.dx=o.dx;a.dy=o.dy;unit(a.dx,a.dy,a.face,0);if(a.dx>.05)a.face=1;else if(a.dx<-.05)a.face=-1;}
 void start(int i,const Order&o,const Config&c){F&a=f[i];int s=o.skill;if(!s||a.meter+1e-9<c.m[s].cost||(s==SLAM&&a.ground)||(s==HOOK&&a.cd>0))return;a.dx=a.face;a.dy=0;setdir(a,o);a.act=s;a.meter-=c.m[s].cost;a.mask=0;a.charge=0;a.power=1;
 if(s==CHARGE){a.phase=CHARGING;a.pt=0;}else if(s==GUARD){a.phase=ACTIVE;a.pt=60;}else if(s==BLINK)active(i,c);else{a.phase=WINDUP;a.pt=c.m[s].windup;}}
 void release(F&a,const Config&c){a.power=1+std::min(1.,a.charge/45.);a.phase=WINDUP;a.pt=c.m[CHARGE].windup;}
 void advance(int i,const Config&c){F&a=f[i];if(a.phase==CHARGING){if(++a.charge>=75)release(a,c);}else if(a.phase==WINDUP){if(--a.pt<=0)active(i,c);}else if(a.phase==ACTIVE){if(--a.pt<=0){if(a.act==GUARD)end(a,30);else recover(i,c);}}else if(a.phase==RECOVER&&--a.pt<=0)end(a,c.gap);}
 void control(int i,const Order&o,bool first,const Config&c){F&a=f[i];if(a.respawn){if(--a.respawn==0){a.x=(i-(n-1)/2.)*1.5;a.y=13;a.vx=a.vy=0;a.ground=false;a.drop=0;a.air=1;a.invuln=c.invuln;}return;}
 if(a.invuln>0)--a.invuln;if(a.cd>0)--a.cd;if(a.drop>0)--a.drop;a.meter=std::min(1.,a.meter+c.refill);if(a.pull>0)return;if(a.stun>0){if(--a.stun==0)a.gap=0;return;}
 bool released=false;if(a.act==GUARD&&a.phase==ACTIVE&&o.skill!=GUARD)end(a,c.gap);else if(a.act==CHARGE&&a.phase==CHARGING){setdir(a,o);if(o.skill==GUARD||o.skill==BLINK)end(a,0);else if(o.skill!=CHARGE){release(a,c);released=true;}}
 if(!a.act){if(a.gap>0)--a.gap;else start(i,o,c);}else if(!released)advance(i,c);
 if(first&&o.jump&&(!a.act||a.act==CHARGE)){if(a.ground){a.vy=c.jump;a.ground=false;}else if(a.air>0){--a.air;a.vy=c.airjump;}}
 if(o.drop&&a.ground)a.drop=10;
 if(a.act==GUARD){if(a.ground)a.vx=0;return;}if((a.act==BLINK&&a.phase==ACTIVE)||a.reel)return;
 if(a.act&&a.act!=CHARGE){if(a.ground)a.vx*=c.gfric;return;}
 double want=o.move*c.run*(a.act==CHARGE?.25:1);double accel=a.ground?c.gacc:c.aacc;a.vx+=clamp(want-a.vx,-accel,accel);if(!a.act){if(o.move>.05)a.face=1;else if(o.move<-.05)a.face=-1;}}
 bool overlap(const F&a,const Config&c)const{return a.x+c.half>c.x1&&a.x-c.half<c.x2&&a.y<c.top&&a.y+c.height>c.bottom;}
 void land(int i,double y,const Config&c){F&a=f[i];a.y=y;a.vy=0;a.ground=true;if(a.slamLand){if(c.slam_cap>=0)a.stun=std::min(a.stun,c.slam_cap);a.slamLand=false;}a.air=1;if((a.act==SLAM||a.act==UPPER)&&a.phase==ACTIVE)recover(i,c);}
 void motion(int i,const Config&c){F&a=f[i];if(a.reel){double dx=a.hx-a.x,dy=a.hy-(a.y+c.height/2);if(dx*dx+dy*dy<=2.56){a.vx=0;a.vy=.2;recover(i,c);}else{unit(dx,dy,0,1);a.vx=dx*.9;a.vy=dy*.9;}}
 if(a.pull>0){F&b=f[a.pulled];double dx=b.x-a.x,dy=b.y-a.y;--a.pull;if(dx*dx+dy*dy<=2.56||b.out||b.respawn){a.vx=a.vy=0;a.pull=0;a.pulled=-1;}else{unit(dx,dy);a.vx=dx*.3;a.vy=dy*.3;}}
 bool floating=(a.act==BLINK&&a.phase==ACTIVE)||a.reel||a.pull>0;if(!floating){a.vy=std::max(a.vy-c.gravity,a.stun>0?-2.:-c.maxfall);if(a.stun>0){a.vx*=c.hfric;a.vy*=c.hfric;}else if(!a.ground)a.vx*=c.afric;}
 a.x+=a.vx;if(overlap(a,c)){a.x=a.vx>0?c.x1-c.half:c.x2+c.half;a.vx=a.stun>0?-a.vx*c.bounce:0;}
 double foot=a.y;a.y+=a.vy;a.ground=false;if(overlap(a,c)){if(a.vy<=0)land(i,c.top,c);else{a.y=c.bottom-c.height;a.vy=a.stun>0?-a.vy*c.bounce:0;}}
 if(a.vy<=0&&a.drop==0&&!a.ground)for(int p=0;p<c.np;p++){auto&pl=c.platforms[p];if(foot>=pl[2]&&a.y<=pl[2]&&a.x+c.half>pl[0]&&a.x-c.half<pl[1]){land(i,pl[2],c);break;}}}
 void launch(int a,int t,double dx,double dy,double dmg,double power,int sk,const Config&c){F&v=f[t];unit(dx,dy,f[a].x<=v.x?1:-1,0);v.damage+=dmg;dealt[a]+=dmg;received[t]+=dmg;tags[a]|=1<<t;double speed=(c.base+c.scale*v.damage/100)*power;cancel(t);v.pull=0;v.pulled=-1;v.vx=dx*speed;v.vy=dy*speed;v.ground=false;v.slamLand=(sk==SLAM&&c.slam_cap>=0);v.stun=std::min(c.stunmax,c.stunbase+int(speed*c.stunper));v.gap=0;v.last=a;v.lasttick=tick;}
 void counterback(int t,int a,double dmg,double power,const Config&c){f[t].phase=RECOVER;f[t].pt=4;launch(t,a,f[a].x-f[t].x,.4,dmg*2,power*2,COUNTER,c);}
 void strike(const Hit&h,const Config&c){F&t=f[h.t];if(untouch(t))return;if(h.s!=GRAB){if(h.s!=SLAM&&counter(t)){counterback(h.t,h.a,h.damage,h.power,c);return;}if(guard(t)){t.vx=(f[h.a].x<=t.x?1:-1)*.12;return;}}launch(h.a,h.t,h.dx,h.dy,h.damage,h.power,h.s,c);}
 void melee(const Config&c){Hit hits[16];int nh=0;for(int a=0;a<n;a++){F&v=f[a];if(v.out||v.respawn||v.phase!=ACTIVE||!(v.act==ATTACK||v.act==CHARGE||v.act==UPPER||v.act==SLAM||v.act==GRAB))continue;auto&m=c.m[v.act];double ax=v.x+v.dx*m.offset,ay=v.y+v.dy*m.offset,reach=m.radius+c.height/2;
 for(int t=0;t<n;t++){if(t==a||(v.mask>>t&1)||untouch(f[t]))continue;if(sq(f[t].x-ax)+sq(f[t].y-ay)>sq(reach))continue;v.mask|=1<<t;double scale=v.act==CHARGE?v.power:1;hits[nh++]={a,t,v.act,v.dx,v.dy+m.lift,m.damage*scale,m.power*scale*(v.act==CHARGE?1+(c.charge_short-1)*(1-std::min(1.,(v.charge/45.)/c.charge_full)):1)};}}for(int h=0;h<nh;h++)strike(hits[h],c);}
 void miss(int owner,const Config&c){f[owner].cd=150;if(f[owner].act==HOOK)recover(owner,c);}
 void projectiles(const Config&c){for(int k=0;k<ns;k++){Shot&s=shots[k];if(s.dead)continue;s.x+=s.vx;s.y+=s.vy;--s.ttl;
 if(s.x>c.x1&&s.x<c.x2&&s.y<c.top&&s.y>c.bottom){if(s.kind==HOOK&&f[s.owner].act==HOOK){F&a=f[s.owner];a.reel=true;a.hx=s.x;a.hy=s.y;a.phase=ACTIVE;a.pt=30;}s.dead=true;continue;}
 double reach=c.m[s.kind].radius+c.height/2;
 for(int t=0;t<n&&!s.dead;t++){if(t==s.owner||untouch(f[t]))continue;F&v=f[t];if(sq(v.x-s.x)+sq(v.y+c.height/2-s.y)>sq(reach))continue;
 if(s.kind==HOOK){cancel(t);v.slamLand=false;v.damage+=c.m[HOOK].damage;dealt[s.owner]+=c.m[HOOK].damage;received[t]+=c.m[HOOK].damage;tags[s.owner]|=1<<t;v.pulled=s.owner;v.pull=30;v.stun=0;v.ground=false;v.last=s.owner;v.lasttick=tick;if(f[s.owner].act==HOOK)recover(s.owner,c);s.dead=true;break;}
 if(counter(v)){v.phase=RECOVER;v.pt=4;s.owner=t;s.vx=-s.vx;s.vy=-s.vy;s.damage*=2;s.power*=2;s.ttl=40;break;}if(guard(v)){s.dead=true;break;}launch(s.owner,t,s.vx,s.vy+.2,s.damage,s.power,WAVE,c);s.dead=true;}
 if(!s.dead&&s.ttl<=0){if(s.kind==HOOK)miss(s.owner,c);s.dead=true;}}}
 double bx(const Config&c)const{return std::max(c.min_x,c.blastx-std::max(0,tick-c.shrinkstart)*c.shrink);}double bt(const Config&c)const{return std::max(c.min_top,c.blasttop-std::max(0,tick-c.shrinkstart)*c.shrink*c.topratio);}
 void blast(int i,const Config&c){F&a=f[i];if(std::abs(a.x)<=bx(c)&&a.y<=bt(c)&&a.y>=c.blastbottom)return;if(a.last>=0&&a.last!=i&&tick-a.lasttick<=c.credit)++kills[a.last];--a.stocks;cancel(i);a.slamLand=false;a.damage=a.vx=a.vy=0;a.stun=a.gap=a.pull=0;a.pulled=-1;a.last=-1;a.lasttick=-9999;if(a.stocks<=0){a.out=true;a.stocks=0;}else a.respawn=c.respawn;}
 void step(const std::array<Order,4>&os,bool first,const Config&c,bool rounding=false){++tick;for(int i=0;i<n;i++)if(!f[i].out)control(i,os[i],first,c);for(int i=0;i<n;i++)if(!f[i].out&&!f[i].respawn)motion(i,c);projectiles(c);melee(c);for(int i=0;i<n;i++)if(!f[i].out&&!f[i].respawn)blast(i,c);
 int w=0;for(int k=0;k<ns;k++)if(!shots[k].dead)shots[w++]=shots[k];ns=w;
 if(rounding){auto rnd=[](double x){return std::round(x*10000)/10000;};for(auto&a:f){a.x=rnd(a.x);a.y=rnd(a.y);a.vx=rnd(a.vx);a.vy=rnd(a.vy);a.meter=rnd(a.meter);a.damage=rnd(a.damage);}for(int k=0;k<ns;k++){auto&s=shots[k];s.x=rnd(s.x);s.y=rnd(s.y);}}
 }
};
inline State parse(const J&o,const Config&c){State s;s.n=o.at("players").size();s.tick=o.at("tick").get<int>();for(int i=0;i<s.n;i++){auto&j=o.at("players").at(i);F&a=s.f[i];a.x=j.at("x").as_double();a.y=j.at("y").as_double();a.vx=j.at("vx").as_double();a.vy=j.at("vy").as_double();a.damage=j.at("damage").as_double();a.meter=j.at("meter").as_double();a.dx=j.at("dir").at(0).as_double();a.dy=j.at("dir").at(1).as_double();a.face=j.at("facing").get<int>();a.air=j.at("air_jumps").get<int>();a.stocks=j.at("stocks").get<int>();a.respawn=j.at("respawn").get<int>();a.invuln=j.at("invuln").get<int>();a.stun=j.at("hitstun").get<int>();a.gap=j.at("gap").get<int>();a.cd=j.at("hook_cd").get<int>();a.ground=j.at("grounded").as_bool();a.out=j.at("out").as_bool();a.act=skill(j.at("act"));std::string p=j.at("phase").as_string();a.phase=p=="charging"?1:p=="windup"?2:p=="active"?3:p=="recover"?4:0;a.pt=j.at("phase_ticks").get<int>();a.charge=j.at("charge").get<int>();a.power=1+std::min(1.,a.charge/45.);}
 for(auto&j:o.at("shots").as_array()){if(s.ns>=32)break;Shot&b=s.shots[s.ns++];b.id=j.at("id").get<int>();b.owner=j.at("owner").get<int>();b.kind=skill(j.at("kind"));b.ttl=j.at("ttl").get<int>();b.x=j.at("x").as_double();b.y=j.at("y").as_double();b.vx=j.at("vx").as_double();b.vy=j.at("vy").as_double();b.damage=c.m[b.kind].damage;b.power=c.m[b.kind].power;s.next=std::max(s.next,b.id+1);}
 return s;}
}
