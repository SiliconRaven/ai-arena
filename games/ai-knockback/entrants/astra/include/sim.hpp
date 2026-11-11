#pragma once
#ifndef ARENA_JSON_READY
#include "json.hpp"
#endif
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
using J=mini_json::Json;
namespace arena_bot {
constexpr double PI=3.14159265358979323846;
inline double sq(double x){return x*x;}
inline double clamp(double x,double a,double b){return std::max(a,std::min(b,x));}
struct V {double x=0,y=0; V()=default; V(double X,double Y):x(X),y(Y){} V operator+(V b)const{return {x+b.x,y+b.y};} V operator-(V b)const{return {x-b.x,y-b.y};} V operator*(double a)const{return {x*a,y*a};} V operator/(double a)const{return {x/a,y/a};} V operator-()const{return {-x,-y};} V& operator+=(V b){x+=b.x;y+=b.y;return *this;} V& operator-=(V b){x-=b.x;y-=b.y;return *this;} V& operator*=(double a){x*=a;y*=a;return *this;} double norm2()const{return x*x+y*y;} double len()const{return std::sqrt(norm2());} double maxabs()const{return std::max(std::abs(x),std::abs(y));} V unit()const{double d=len();return d>1e-10?*this/d:V{};} V perp()const{return {-y,x};}};
inline double dot(V a,V b){return a.x*b.x+a.y*b.y;}
inline V angle(double t){return {std::cos(t),std::sin(t)};}
inline V rotate(V a,double t){return {a.x*std::cos(t)-a.y*std::sin(t),a.x*std::sin(t)+a.y*std::cos(t)};}
inline double rnd(double x){return std::nearbyint(x*10000.)*.0001;}
inline V jv(const J&j){return {j.at(0).as_double(),j.at(1).as_double()};}
inline J vj(V v){return J::array{v.x,v.y};}
inline double num(const J&j,const char*k){return j.at(k).as_double();}
inline int integer(const J&j,const char*k){return j.at(k).get<int>();}
struct Body {V p,v;};
struct Player:Body {double bar=1,charge=0;int skill=0,pause=0,held=0,act=0,actt=0,acd=0,hcd=0,bcd=0;V aim;bool out=false;int hit=-1,hitt=-9999,kills=0,deatht=0;};
struct Bomb:Body {int id=0,fuse=0,owner=0;bool live=true;};
struct Wall {V a,b,e;double invlen=0,minx=0,maxx=0,miny=0,maxy=0,edge=0;int id=0,back=0;bool spring=false,live=true,fallen=false;V nearest(V p)const{return a+e*clamp(dot(p-a,e)*invlen,0,1);} void init(){e=b-a;invlen=1./std::max(1e-20,e.norm2());minx=std::min(a.x,b.x);maxx=std::max(a.x,b.x);miny=std::min(a.y,b.y);maxy=std::max(a.y,b.y);edge=std::max(a.maxabs(),b.maxabs());}};
struct Order {V move,release,attack,bomb;int charge=0,fuse=24;bool operator==(const Order&o)const{return (move-o.move).norm2()<1e-8&&(release-o.release).norm2()<1e-8&&(attack-o.attack).norm2()<1e-8&&(bomb-o.bomb).norm2()<1e-8&&charge==o.charge&&fuse==o.fuse;}};
struct World {
 std::array<Player,8> p;std::array<Bomb,40> b;std::array<Wall,40> w;int n=0,nb=0,nw=0,tick=0,you=0,nextbomb=0;double half=16.5;bool done=false;
 double initialhalf=16.5,shrinkstep=2.5,minhalf=.5;int firstshrink=25,every=25,shrinkturns=10;
 double halfAt(int t)const{int turn=(t-1)/6+1; if(turn<firstshrink)return initialhalf;int stage=(turn-firstshrink)/every;double from=std::max(minhalf,initialhalf-stage*shrinkstep),to=std::max(minhalf,from-shrinkstep);int start=(firstshrink+stage*every-1)*6;return rnd(from+(to-from)*clamp(double(t-start)/(shrinkturns*6),0,1));}
 int alive()const{int a=0;for(int i=0;i<n;++i)a+=!p[i].out;return a;}
 static World read(const J&o){World s;s.n=integer(o,"n");s.you=integer(o,"you");s.tick=integer(o,"tick");s.half=num(o.at("arena"),"half");auto&r=o.at("rules");s.initialhalf=num(r.at("arena"),"half");auto&sh=r.at("shrink");s.shrinkstep=num(sh,"step");s.minhalf=num(sh,"min_half");s.firstshrink=integer(sh,"first_turn");s.every=integer(sh,"every");s.shrinkturns=integer(sh,"shrink_turns");
 for(int i=0;i<s.n;++i){auto&x=o.at("players").at(i);auto&p=s.p[i];p.p={num(x,"x"),num(x,"y")};p.v={num(x,"vx"),num(x,"vy")};p.bar=num(x,"bar");p.charge=num(x,"charge");p.skill=x.at("skill")==J("dash")?1:x.at("skill")==J("hammer")?2:0;p.pause=integer(x,"pause");p.held=integer(x,"held");p.act=x.at("act")==J("recover")?1:x.at("act")==J("jab")?2:x.at("act")==J("jab_recover")?3:0;p.actt=integer(x,"act_ticks");p.acd=integer(x,"attack_cd");p.hcd=integer(x,"hammer_cd");p.bcd=integer(x,"bomb_cd");p.out=x.at("out").as_bool();if(!x.at("aim").is_null())p.aim=jv(x.at("aim"));auto&hit=o.at("last_hit").at(i);if(!hit.is_null()){p.hit=hit.at(0).get<int>();p.hitt=hit.at(1).get<int>();}p.kills=o.at("kills").at(i).get<int>();}
 if(o.contains("bombs"))for(auto&x:o.at("bombs").as_array()){auto&b=s.b[s.nb++];b.p={num(x,"x"),num(x,"y")};b.v={num(x,"vx"),num(x,"vy")};b.id=integer(x,"id");b.fuse=integer(x,"fuse");b.owner=integer(x,"owner");s.nextbomb=std::max(s.nextbomb,b.id+1);}
 for(int broken=0;broken<2;++broken)for(auto&x:o.at(broken?"broken":"walls").as_array()){auto&w=s.w[s.nw++];w.a={num(x,"x1"),num(x,"y1")};w.b={num(x,"x2"),num(x,"y2")};w.id=integer(x,broken?"wall":"id");w.spring=x.at("kind")==J("spring");w.live=!broken;if(broken)w.back=integer(x,"at");w.init();}std::sort(s.w.begin(),s.w.begin()+s.nw,[](const Wall&a,const Wall&b){return a.id<b.id;});return s;}
 void mark(int i,int hitter){p[i].hit=hitter;p[i].hitt=tick;}
 void swing(int i,V aim,bool hammer,double power){auto&a=p[i];double reach=hammer?3.2:2.8,gain=(hammer?.9:.35)*power;for(int j=0;j<n;++j)if(j!=i&&!p[j].out){V d=p[j].p-a.p;double d2=d.norm2();if(d2>reach*reach)continue;V u=d2>1e-20?d/std::sqrt(d2):aim;if(dot(u,aim)<.5)continue;p[j].v+=u*gain;mark(j,i);}for(int j=0;j<nb;++j)if(b[j].live){V d=b[j].p-a.p;double d2=d.norm2();if(d2>reach*reach)continue;V u=d2>1e-20?d/std::sqrt(d2):aim;if(dot(u,aim)<.5)continue;b[j].v+=u*(gain*2);b[j].owner=i;}a.v-=aim*((hammer?.15:.05)*power);}
 void act(int i,const Order&o,bool first){auto&a=p[i];a.acd=std::max(0,a.acd-1);a.hcd=std::max(0,a.hcd-1);bool busy=a.act!=0;if(busy){if(--a.actt<=0){if(a.act==2){swing(i,a.aim,false,1);a.act=3;a.actt=6;a.aim={};}else {a.act=0;a.actt=0;}}}int want=o.charge,skill=a.skill;if(want==2&&a.hcd>0)want=0;bool release=!busy&&first&&skill&&a.charge>0&&o.release.norm2()>0; if(!busy&&!release&&want&&skill&&skill!=want){a.charge=0;a.skill=0;}bool jab=first&&!busy&&o.attack.norm2()>0&&a.acd==0&&!(release&&skill==2);bool charging=!busy&&!release&&!jab&&want&&a.bar>0;
 if(a.pause>0)--a.pause;else if(!charging)a.bar=std::max(a.bar,std::min(1-a.charge,a.bar+.02));
 if(release){double power=a.charge;a.charge=0;a.skill=0;a.pause=30;if(skill==1)a.v+=o.release*(.9*power);else{a.hcd=60;swing(i,o.release,true,power);a.act=1;a.actt=5;a.aim={};}}
 if(jab){a.act=2;a.actt=9;a.aim=o.attack;a.acd=15;}
 if(charging){double amount=std::min(.04,a.bar);a.bar-=amount;a.charge+=amount;a.skill=want;}
 if(a.charge<=0||charging)a.held=0;else if(++a.held>=45){a.charge=0;a.skill=0;a.held=0;a.pause=30;}
 if(!busy&&!a.act)a.v+=o.move*(.018*(a.skill==1?.2:a.skill==2?.3:1));
 if(a.bcd>0)--a.bcd;else if(first&&o.bomb.norm2()>0&&!busy&&!a.act&&nb<40){auto&q=b[nb++];q.live=true;q.p=a.p+o.bomb*1.8;q.v=a.v+o.bomb*.5;q.fuse=o.fuse;q.owner=i;q.id=nextbomb++;a.bcd=90;}
 }
 static double contact(Body&a,Body&b,double reach,double ma,double mb){V d=b.p-a.p;double d2=d.norm2();if(d2>=reach*reach)return 0;double len=std::sqrt(d2);V u=len>0?d/len:V{1,0};double share=mb/(ma+mb);a.p-=u*((reach-len)*share);b.p+=u*((reach-len)*(1-share));double approach=dot(a.v-b.v,u);if(approach<=0)return 0;double imp=1.9*approach/(1/ma+1/mb);a.v-=u*(imp/ma);b.v+=u*(imp/mb);return imp;}
 static void bounce(Body&a,double radius,const Wall&w,bool regen=false){double reach=radius+.25;if(a.p.x<w.minx-reach||a.p.x>w.maxx+reach||a.p.y<w.miny-reach||a.p.y>w.maxy+reach)return;V q=w.nearest(a.p),d=a.p-q;double d2=d.norm2();if(d2>=reach*reach)return;V u=d2>1e-20?d/std::sqrt(d2):w.e.perp().unit();a.p=q+u*reach;double inward=dot(a.v,u);if(regen){a.v+=u*(.5-inward);return;}if(inward>=0)return;double speed=w.spring?std::min(1.5*-inward,.5):.8*-inward;a.v+=u*(speed-inward);}
 void explode(int k){Bomb dead=b[k];b[k].live=false;for(int i=0;i<n;++i)if(!p[i].out){V d=p[i].p-dead.p;double len=d.len();if(len>3.5)continue;V u=len>1e-15?d/len:V{1,0};p[i].v+=u*(1.4*(1-len/3.5));mark(i,dead.owner);}for(int i=0;i<nb;++i)if(b[i].live){V d=b[i].p-dead.p;double len=d.len();if(len>3.5)continue;b[i].v+=(len>1e-15?d/len:V{1,0})*(2.8*(1-len/3.5));}for(int i=0;i<nw;++i)if(w[i].live&&!w[i].spring&&(w[i].nearest(dead.p)-dead.p).norm2()<=12.25){w[i].live=false;w[i].back=tick+300;}}
 static void friction(Body&a,bool bomb){V v=a.v;a.v*=bomb?.95:.94;if(a.v.norm2()<.000001)a.v={};a.p={rnd(a.p.x),rnd(a.p.y)};a.v={rnd(a.v.x),rnd(a.v.y)};if(a.v.x==v.x)a.v.x=0;if(a.v.y==v.y)a.v.y=0;}
 void stepTick(const std::array<Order,8>&orders,bool first){if(done)return;++tick;half=halfAt(tick);
 for(int k=0;k<nw;++k){auto&wall=w[k];if(wall.fallen)continue;if(wall.edge>half){wall.fallen=true;wall.live=false;continue;}if(!wall.live&&wall.back>0&&tick>=wall.back){wall.live=true;wall.back=0;for(int i=0;i<n;++i)if(!p[i].out)bounce(p[i],1,wall,true);for(int i=0;i<nb;++i)if(b[i].live)bounce(b[i],.6,wall,true);}}
 for(int i=0;i<nb;++i)if(b[i].live&&--b[i].fuse<=0)explode(i);
 for(int i=0;i<n;++i)if(!p[i].out)act(i,orders[i],first);
 for(int i=0;i<n;++i)if(!p[i].out)p[i].p+=p[i].v;for(int i=0;i<nb;++i)if(b[i].live)b[i].p+=b[i].v;
 for(int i=0;i<n;++i)if(!p[i].out)for(int j=i+1;j<n;++j)if(!p[j].out&&contact(p[i],p[j],2,1,1)>=.03){mark(i,j);mark(j,i);}
 for(int i=0;i<n;++i)if(!p[i].out)for(int j=0;j<nb;++j)if(b[j].live){bool hitter=dot(p[i].v+b[j].v,b[j].p-p[i].p)>0;if(contact(p[i],b[j],1.6,1,.5)>=.03&&hitter)b[j].owner=i;}
 for(int i=0;i<nb;++i)if(b[i].live)for(int j=i+1;j<nb;++j)if(b[j].live)contact(b[i],b[j],1.2,.5,.5);
 for(int i=0;i<n;++i)if(!p[i].out)for(int k=0;k<nw;++k)if(w[k].live)bounce(p[i],1,w[k]);for(int i=0;i<nb;++i)if(b[i].live)for(int k=0;k<nw;++k)if(w[k].live)bounce(b[i],.6,w[k]);
 for(int i=0;i<n;++i)if(!p[i].out){friction(p[i],false);p[i].bar=rnd(p[i].bar);p[i].charge=rnd(p[i].charge);}for(int i=0;i<nb;++i)if(b[i].live)friction(b[i],true);
 for(int i=0;i<n;++i)if(!p[i].out&&p[i].p.maxabs()>half){p[i].out=true;p[i].v={};p[i].act=0;p[i].actt=0;p[i].deatht=tick;if(p[i].hit>=0&&p[i].hit!=i&&tick-p[i].hitt<=90)p[p[i].hit].kills++;}
 int bb=0;for(int i=0;i<nb;++i)if(b[i].live&&b[i].p.maxabs()<=half)b[bb++]=b[i];nb=bb;done=alive()<=1;
 }
 void step(const std::array<Order,8>&orders){for(int k=0;k<6&&!done;++k)stepTick(orders,k==0);}
};
inline J jsonOrder(const Order&o,const Player&p){J j=J::object{};if(o.move.norm2()>0)j["move"]=vj(o.move);if(o.charge)j["charge"]=o.charge==1?"dash":"hammer";if(o.release.norm2()>0&&p.skill)j[p.skill==1?"dash":"hammer"]=vj(o.release);if(o.attack.norm2()>0)j["attack"]=vj(o.attack);if(o.bomb.norm2()>0)j["bomb"]=J{{"dir",vj(o.bomb)},{"fuse",o.fuse}};return j;}
}
