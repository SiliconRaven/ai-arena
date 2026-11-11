#pragma once
#include "sim.hpp"
#include <chrono>
#include <vector>
#include <numeric>
#ifndef SEARCH_HORIZON
#define SEARCH_HORIZON 42
#endif
#ifndef SEARCH_SHORT
#define SEARCH_SHORT 15
#endif
#ifndef SEARCH_KEEP
#define SEARCH_KEEP 14
#endif
#ifndef DAMAGE_AVERSION
#define DAMAGE_AVERSION 1.0
#endif
#ifndef DAMAGE_REWARD
#define DAMAGE_REWARD 1.0
#endif
#ifndef POSITION_WEIGHT
#define POSITION_WEIGHT 1.0
#endif
#ifndef BLINK_PENALTY
#define BLINK_PENALTY 0
#endif
#ifndef KILL_VALUE
#define KILL_VALUE 86.0
#endif
#ifndef TAG_VALUE
#define TAG_VALUE 2.0
#endif
#ifndef MODEL_COUNT
#define MODEL_COUNT 3
#endif
namespace bot {
using namespace fast;
struct Style {double attacks=1,guards=.3,counters=.3,grabs=.4,slams=.4,hooks=.3,charges=.3,waves=.2,blinks=.2; double move=0;int lastact=0;};
struct Candidate{Order o;int hold=0;int steer=0;double cheap=0;double deep=0;};
struct Brain{
 Config c;bool initialized=false;State prev;bool hasprev=false;Order prevorder;std::array<Style,4> style;int me=0,target=0,nowtick=0;long turn=0;
 std::array<double,4> lastMove{};std::array<int,4> moveTick{};
 std::chrono::steady_clock::time_point begin;
 double elapsed()const{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();}
 bool live(const F&a)const{return !a.out&&!a.respawn;}
 int nearest(const State&s,int i,bool pick=false)const{int best=-1;double bd=1e30;auto&a=s.f[i];for(int j=0;j<s.n;j++){auto&b=s.f[j];if(i==j||!live(b))continue;double dx=b.x-a.x,dy=b.y-a.y;double v=std::sqrt(dx*dx+dy*dy*1.1);if(pick){v+=b.invuln>12?9:0;v-=std::min(3.5,b.damage*.025);v+=std::max(0.,std::abs(b.x)-12)*.6;if(b.stun>5)v-=1.2;if(j==target)v-=.35;}if(v<bd){bd=v;best=j;}}return best;}
 bool ready(const F&a)const{return a.stun==0&&a.pull==0&&a.gap<=2&&a.act==0;}
 double surface(double x)const{double best=0;for(int p=0;p<c.np;p++){auto&pl=c.platforms[p];if(x>pl[0]-.4&&x<pl[1]+.4)best=std::max(best,pl[2]);}return best;}
 bool needsRecovery(const State&s,int i)const{const F&a=s.f[i];return live(a)&&!a.ground&&(a.y<-.2||std::abs(a.x)>12.7);}
 Order recovery(const State&s,int i)const{
 const F&a=s.f[i];double toward=-sign(a.x),dist=std::max(0.,std::abs(a.x)-12.3);Order o=order(0,toward,0,toward);
 if(a.stun||a.pull)return o;
#ifdef RECOVERY_V3
 if(a.y<-5.5&&std::abs(a.x)<13.6){
 double out=sign(a.x);o=order(0,out,.15,out);
 if((!a.act||a.act==CHARGE)&&a.gap<=0&&a.meter>=.2){o=order(BLINK,out,.3,out);return o;}
 if(!a.act&&a.air>0&&a.vy<0&&a.y<-8)o.jump=true;
 return o;
 }
#endif
 if(!a.act||a.act==CHARGE){
 if(a.air>0&&a.vy<.08&&(a.y<1.5||dist>3.5)){o.jump=true;return o;}
 if(a.gap>0)return o;
 if(a.y<-.5&&a.vy<.12&&a.meter>=.2){if(dist>3.5||a.y<-6.5){o.skill=BLINK;o.dx=toward*(dist>5?.9:.55);o.dy=.85;unit(o.dx,o.dy);return o;}if(a.meter>=.25){o.skill=UPPER;o.dx=0;o.dy=1;return o;}}
 if(a.y>-.2&&dist>4.0&&a.vy<.1&&a.meter>=.2){o.skill=BLINK;o.dx=toward;o.dy=.35;unit(o.dx,o.dy);return o;}
 if(a.air==0&&a.y<3&&a.vy<-.2&&dist>.5&&a.meter>=.25){o.skill=UPPER;o.dx=0;o.dy=1;return o;}
 }
 return o;}
 bool hitSoon(const F&a,const F&b,int frames=7)const{
 if(b.stun||b.invuln<0||b.phase==RECOVER||b.act==GUARD||b.act==BLINK||b.act==COUNTER||b.act==NONE||b.act==WAVE||b.act==HOOK)return false;
 if(b.phase==CHARGING)return std::abs(a.x-b.x)<4.5&&std::abs(a.y-b.y)<3;
 int dt=b.phase==WINDUP?b.pt:0;if(dt>frames)return false;
 auto&m=c.m[b.act];double bx=b.x+b.vx*dt*(b.ground?.25:.9),by=b.y;
 if(!b.ground)by+=b.vy*dt-.0175*dt*dt;
 if(b.act==UPPER)by+=.5;
 if(b.act==SLAM)by-=.5;
 double ax=a.x+a.vx*dt*(a.ground?.25:.8),ay=a.y+(a.ground?0:a.vy*dt-.0175*dt*dt);
 return sq(ax-bx-b.dx*m.offset)+sq(ay-by-b.dy*m.offset)<sq(m.radius+c.height/2+.2);
 }
 Order policy(const State&s,int i,int model,bool self=false)const{
 const F&a=s.f[i];if(!live(a))return Order{};
 if(needsRecovery(s,i))return recovery(s,i);
 int t=nearest(s,i,self
#ifdef TARGET_MODEL
 ||model==2
#endif
 );if(t<0)return order(0,-sign(a.x),0,std::abs(a.x)>1?-sign(a.x):0);
 const F&b=s.f[t];double dx=b.x-a.x,dy=b.y-a.y,d=std::sqrt(dx*dx+dy*dy),side=sign(dx);
 Order o=order(0,dx,dy,side);if(std::abs(dx)<1.7)o.move=0;
#ifdef MOTION_MODEL
 if(!self&&model==0&&s.tick-nowtick<9&&nowtick-moveTick[i]<=6&&std::abs(dx)>2.3)o.move=lastMove[i];
#endif
 if(a.y>3&&dy<-2)o.drop=true;
 if(std::abs(a.x)>std::min(11.5,s.bx(c)-2)&&o.move==sign(a.x))o.move=-sign(a.x);
 if(a.act==GUARD){bool danger=false,grab=false;for(int j=0;j<s.n;j++)if(i!=j&&live(s.f[j])){danger|=hitSoon(a,s.f[j],9);grab|=(s.f[j].act==GRAB||s.f[j].act==HOOK)&&std::abs(s.f[j].x-a.x)<5;}
 if(danger&&!grab&&a.pt>8)o.skill=GUARD;return o;}
 if(a.act==CHARGE&&a.phase==CHARGING){o.dx=dx;o.dy=dy;unit(o.dx,o.dy);if(d>4.0&&a.charge<24){o.skill=CHARGE;o.jump=dy>3&&a.ground;}else if(a.charge<6&&b.stun<10)o.skill=CHARGE;return o;}
 #ifdef STRONG_MODEL
 if(a.act||a.gap>2||a.stun>2||a.pull>2)return o;
#else
 if(a.act||a.gap>2||a.stun||a.pull)return o;
#endif
 // Reactions cannot use a move until it appears in the observation.
 bool incoming=false,unblock=false,slam=false;int enemy=-1;for(int j=0;j<s.n;j++)if(j!=i&&live(s.f[j])&&hitSoon(a,s.f[j],6)){incoming=true;enemy=j;unblock|=s.f[j].act==GRAB;slam|=s.f[j].act==SLAM;}
 for(int k=0;k<s.ns;k++){auto&sh=s.shots[k];if(sh.owner==i)continue;double dx0=sh.x-a.x,dy0=sh.y-a.y-.9;double vv=sh.vx*sh.vx+sh.vy*sh.vy;double time=clamp(-(dx0*sh.vx+dy0*sh.vy)/std::max(.01,vv),0,7);if(sq(dx0+sh.vx*time)+sq(dy0+sh.vy*time)<2.5&&time<6){incoming=true;unblock|=sh.kind==HOOK;}}
 const Style&st=style[i];bool defender=model==1||(model==0&&(st.guards+st.counters>st.attacks*.6));bool grappler=model==2||(model==0&&st.grabs>st.attacks*.7);bool aggressive=model==3;
#ifdef EVASIVE_MODEL
 aggressive=false;bool evader=model==3||(model==0&&st.blinks>st.attacks*.8);
 if(incoming&&a.invuln<5&&evader&&a.meter>=.2){double bx=-side;if(std::abs(a.x)>8)bx=-sign(a.x);return order(BLINK,bx,.2,bx);}
#endif
 if(incoming&&a.invuln<5&&!aggressive){if(unblock&&a.meter>=.2){o=order(BLINK,dx>0?-1:1,.25,-side);if(std::abs(a.x)>9)o.dx=-sign(a.x);unit(o.dx,o.dy);return o;}if(!unblock){if(!slam&&a.meter>=.3&&enemy>=0&&
#ifdef STRONG_MODEL
 ((s.f[enemy].phase==WINDUP&&s.f[enemy].pt>=3+std::min(2,a.stun))||(s.f[enemy].phase==CHARGING&&s.f[enemy].charge>=2))&&
#else
 s.f[enemy].phase==WINDUP&&s.f[enemy].pt>=3&&
#endif
 (defender||self)){o.skill=COUNTER;return o;}if(defender||self){o.skill=GUARD;return o;}}}
 if(b.invuln>6&&a.invuln<6&&d<5){o.move=-side;if(a.ground&&std::abs(dx)<3)o.jump=true;return o;}
 #ifdef COMBAT_MODEL
 if(a.ground&&a.meter>=.2&&std::abs(dx)<3.65&&dy>-1&&dy<3.8&&!State::guard(b)&&(self||model==1||st.slams>st.attacks*.65)){o.skill=SLAM;o.jump=true;return o;}
#endif
 if(!a.ground&&a.y>b.y+.8&&a.y-b.y<5.7&&std::abs(dx)<3.1&&a.meter>=.2&&!(b.act==GUARD&&b.phase==ACTIVE)){o.skill=SLAM;return o;}
 if(dy>1.0&&dy<5.0&&std::abs(dx)<2.6&&a.meter>=.25){if(b.act!=COUNTER&&b.act!=GUARD){o.skill=UPPER;return o;}}
 if(d<3.15){
 if((State::guard(b)||State::counter(b)||grappler)&&a.meter>=.3){o.skill=GRAB;return o;}
 if(b.stun>12&&d<3.4){o.skill=CHARGE;return o;}
 if(!State::guard(b)&&!State::counter(b)){
#ifdef STRONG_MODEL
 if((model==0&&st.charges>st.attacks*.65)&&b.stun>2){o.skill=CHARGE;return o;}
#endif
 o.skill=ATTACK;return o;}
 if(a.meter>=.4&&a.cd==0){o.skill=HOOK;return o;}
 o.move=-side;return o;
 }
 if((model==0&&st.hooks>st.attacks*.65||model==2)&&d>4&&d<10&&a.meter>.6&&a.cd==0){o.skill=HOOK;return o;}
 if(model==0&&st.waves>st.attacks*.6&&d>8&&d<17&&a.meter>.8){o.skill=WAVE;return o;}
 if(dy>2.0&&a.ground&&std::abs(dx)<7)o.jump=true;
 if(!a.ground&&a.air>0&&dy>3&&a.vy<.05&&std::abs(dx)<3)o.jump=true;
 if(self&&b.invuln>20){o.move=std::abs(a.x)>4?-sign(a.x):0;}
 return o;
 }
 void inferHidden(State&s){
 std::array<Order,4> os;std::array<std::vector<double>,4> moves;std::array<int,4> others{};int no=0;
 for(int i=0;i<s.n;i++){
 const F&a=s.f[i];const F&b=prev.f[i];Order o=order(0,a.dx,a.dy);
 if(i==me){os[i]=prevorder;moves[i]={o.move};continue;}
 if(a.act&&a.act!=b.act)o.skill=a.act;
 else if(a.act==GUARD&&a.phase==ACTIVE)o.skill=GUARD;
 else if(a.act==CHARGE&&a.phase==CHARGING)o.skill=CHARGE;
 else if(a.act&&a.act!=CHARGE)o.skill=a.act;
 o.jump=(a.air<b.air&&a.stocks==b.stocks)||(b.ground&&!a.ground&&a.vy>.12&&a.damage<=b.damage+.01&&(b.act==NONE||b.act==CHARGE));
 o.drop=b.ground&&b.y>0&&!a.ground&&a.y<b.y-.02&&a.damage<=b.damage+.01;
 os[i]=o;
 bool movable=(b.act==NONE||b.act==CHARGE||b.act==GUARD)&&b.stun<=3&&b.pull<=3&&live(b);
 if(movable){moves[i]={-1,0,1};double m=clamp(a.vx/c.run,-1,1);if(std::abs(m)>.05&&std::abs(m)<.94)moves[i].push_back(m);others[no++]=i;}
 else moves[i]={0};
 }
 State best;std::array<Order,4> bestos;double besterr=1e100;int total=1;for(int k=0;k<no;k++)total*=int(moves[others[k]].size());
 for(int comb=0;comb<total;comb++){
 int code=comb;for(int k=0;k<no;k++){int i=others[k];os[i].move=moves[i][code%moves[i].size()];code/=moves[i].size();}
 State p=prev;for(int t=0;t<3;t++)p.step(os,t==0,c,true);
 double err=0;for(int i=0;i<s.n;i++){const F&a=s.f[i];const F&b=p.f[i];err+=std::min(30.,sq(a.x-b.x)+sq(a.y-b.y))*2+sq(a.vx-b.vx)*3+sq(a.vy-b.vy)*3+std::min(100.,std::abs(a.damage-b.damage))*4+(a.stocks!=b.stocks?200:0)+(a.act!=b.act?2:0)+std::abs(a.stun-b.stun)*.1;}
 if(err<besterr){besterr=err;best=p;bestos=os;if(err<1e-10)break;}
 }
 for(int i=0;i<s.n;i++){
 F&a=s.f[i];const F&p=best.f[i];if(a.stocks!=p.stocks||a.respawn!=p.respawn)continue;
 double err=sq(a.x-p.x)+sq(a.y-p.y)+std::abs(a.damage-p.damage);
 if(err<.15){a.last=p.last;a.lasttick=p.lasttick;a.drop=p.drop;a.slamLand=p.slamLand;a.pulled=p.pulled;a.pull=p.pull;
 if(moves[i].size()>1&&a.stun==0&&a.act==NONE){lastMove[i]=bestos[i].move;moveTick[i]=s.tick;}
 if(a.act==p.act){a.mask=p.mask;a.power=p.power;a.reel=p.reel;a.hx=p.hx;a.hy=p.hy;}}
 }
 for(int k=0;k<s.ns;k++)for(int l=0;l<best.ns;l++)if(s.shots[k].id==best.shots[l].id&&s.shots[k].owner==best.shots[l].owner){s.shots[k].damage=best.shots[l].damage;s.shots[k].power=best.shots[l].power;break;}
 }
 void restore(State&s,const J&memory){
 if(!hasprev&&memory.is_object()&&memory.contains("h")){
 auto&h=memory.at("h");if(h.is_array()&&h.size()==s.n)for(int i=0;i<s.n;i++){auto&v=h.at(i);if(!v.is_array()||v.size()<11)continue;auto&a=s.f[i];a.last=v.at(0).get<int>();a.lasttick=v.at(1).get<int>();if(a.act==v.at(2).get<int>()){a.mask=v.at(3).get<int>();a.power=v.at(4).as_double();}a.pulled=v.at(5).get<int>();a.pull=std::max(0,v.at(6).get<int>()-3);a.reel=v.at(7).as_bool();a.hx=v.at(8).as_double();a.hy=v.at(9).as_double();a.drop=std::max(0,v.at(10).get<int>()-3);if(v.size()>11)a.slamLand=v.at(11).as_bool();}
 }
 if(!hasprev)return;
 for(int i=0;i<s.n;i++){auto&a=s.f[i];const auto&b=prev.f[i];Style&st=style[i];
 if(a.act&&((a.act!=b.act)||(a.phase==WINDUP&&a.pt>b.pt)||(a.phase==CHARGING&&b.phase!=CHARGING))){double*p=nullptr;switch(a.act){case ATTACK:p=&st.attacks;break;case GUARD:p=&st.guards;break;case COUNTER:p=&st.counters;break;case GRAB:p=&st.grabs;break;case SLAM:p=&st.slams;break;case HOOK:p=&st.hooks;break;case CHARGE:p=&st.charges;break;case WAVE:p=&st.waves;break;case BLINK:p=&st.blinks;break;}if(p)*p+=1;}
 st.move=a.vx/c.run;
 if(a.stocks==b.stocks&&a.respawn==0){a.slamLand=b.slamLand&&!a.ground&&a.stun>0;a.last=b.last;a.lasttick=b.lasttick;a.drop=std::max(0,b.drop-3);if(a.act==b.act&&a.act){a.power=b.power;if(a.act==CHARGE)a.power=1+std::min(1.,a.charge/45.);if(a.phase==ACTIVE&&b.phase==ACTIVE&&a.pt<=b.pt)a.mask=b.mask;a.reel=b.reel;a.hx=b.hx;a.hy=b.hy;}
 if(a.damage>b.damage+.1){a.slamLand=c.slam_cap>=0&&std::abs(a.damage-b.damage-13)<.001&&std::abs(a.vx)<.001&&a.vy<0;int atk=-1;double best=1e20;for(int j=0;j<s.n;j++){if(j==i)continue;const auto&o=prev.f[j];if(o.act==0||o.act==GUARD||o.act==BLINK)continue;double dd=sq(o.x-b.x)+sq(o.y-b.y);if(dd<best){best=dd;atk=j;}}for(int k=0;k<prev.ns;k++){auto&sh=prev.shots[k];double dd=sq(sh.x-b.x)+sq(sh.y-b.y-.9);if(sh.owner!=i&&dd<best){best=dd;atk=sh.owner;}}if(atk>=0){a.last=atk;a.lasttick=s.tick;s.f[atk].mask|=1<<i;}
 if(a.damage-b.damage<3.1&&a.damage-b.damage>2.9&&a.stun==0&&atk>=0){a.pulled=atk;a.pull=28;}
 }else if(b.pull>3&&a.stun==0&&a.act==0){a.pulled=b.pulled;a.pull=b.pull-3;if(a.pulled>=0&&sq(a.x-s.f[a.pulled].x)+sq(a.y-s.f[a.pulled].y)<3)a.pull=0;}
 if(a.act==HOOK&&a.phase==ACTIVE&&a.pt<40&&!a.reel){for(int k=0;k<prev.ns;k++)if(prev.shots[k].owner==i){a.reel=true;a.hx=prev.shots[k].x+prev.shots[k].vx*3;a.hy=prev.shots[k].y+prev.shots[k].vy*3;}}
 }
 }
 // Public attack phases plus observed damage establish which victims were already hit.
 for(int i=0;i<s.n;i++)if(s.f[i].phase==ACTIVE)for(int j=0;j<s.n;j++)if(s.f[j].last==i&&s.tick-s.f[j].lasttick<7&&s.f[j].stun>0)s.f[i].mask|=1<<j;
#ifdef INVERSE_STATE
 inferHidden(s);
#endif
 }
 double deathRisk(const State&s,int i)const{
 F a=s.f[i];if(!live(a))return 0;double bx=s.bx(c),bt=s.bt(c);double risk=0;
 if(a.stun>0){double x=a.x,y=a.y,vx=a.vx,vy=a.vy;bool ground=a.ground;for(int k=0;k<std::min(45,a.stun);k++){vx*=.97;vy=std::max(vy-.035,-2.)*.97;x+=vx;y+=vy;if(std::abs(x)>bx||y>bt||y<c.blastbottom)return 1.;if(x>-13.5&&x<13.5&&y<0&&y>-1&&vy<0){y=0;vy=0;ground=true;}if(y<0&&std::abs(x)<13.5&&y>-5.8){vx=-vx*.6;x=sign(x)*13.6;}}
 a.x=x;a.y=y;a.vx=vx;a.vy=vy;a.ground=ground;}
 double outside=std::max(0.,std::abs(a.x)-12.5);if(a.y<-2){double reach=(a.air>0?8.:0)+(a.meter>=.2?10.:0)+(a.meter>=.5?5.:0);risk=clamp((-a.y+outside*.45-2-reach)/8,0,1);if(std::abs(a.x)<12.5&&a.y<-5.6)risk=std::max(risk,.65);}
 if(outside>0){risk=std::max(risk,clamp((outside-3-(a.air?4:0)-(a.meter>=.2?6:0))/8,0,.9));}
 if(std::abs(a.x)>bx-1&&a.vx*sign(a.x)>.2)risk=std::max(risk,.6);
 if(a.y>bt-1.5&&a.vy>.25)risk=std::max(risk,.6);
 return risk;
 }
 double position(const State&s,int i,int tar)const{
 const F&a=s.f[i];if(!live(a))return 0;double v=0,bx=s.bx(c),bt=s.bt(c);
 double edge=std::max(0.,std::abs(a.x)-9.5);v-=edge*edge*.45;
 if(a.y<-.5)v-=8+(-a.y)*2.0+std::max(0.,std::abs(a.x)-12.4)*2;
 if(a.y>bt-4)v-=sq(a.y-bt+4)*1.1;
 v-=std::max(0.,std::abs(a.x)-(bx-4))*4;
 if(a.ground)v+=1.5;
 v+=a.air*.8;v+=a.meter*7;
 if(tar>=0&&live(s.f[tar])){const F&b=s.f[tar];double d=std::sqrt(sq(a.x-b.x)+sq(a.y-b.y)*1.15);if(b.invuln<12||a.invuln>12){v-=std::min(d,24.)*.65;if(a.y>b.y+1&&a.y<b.y+6&&std::abs(a.x-b.x)<4)v+=2;}else if(d<5)v-=(5-d)*2;}
 // High damage makes a central low position disproportionately valuable.
 v-=std::max(0.,a.damage-70)*.0025*(std::abs(a.x)+std::max(0.,a.y-5));
 if(s.tick>3550){v-=std::max(0.,a.y-1)*.65;v-=std::abs(a.x)*.3;}
 return v;
 }
 double evaluate(const State&s,const State&root)const{
 const F&a=s.f[me];const F&r=root.f[me];double score=0;int alive=0;for(int i=0;i<s.n;i++)if(!root.f[i].out)alive++;
 double stockval=150+(r.stocks==1?100:0)+(alive==2?40:0);
 score-=stockval*(r.stocks-a.stocks);score+=s.kills[me]*KILL_VALUE;
 double aversion=DAMAGE_AVERSION;
#ifdef ADAPT_LATE
 if(root.tick>3500)aversion=1.0;
#endif
 score-=s.received[me]*(.9+std::min(150.,r.damage)*.006)*aversion;
 score+=s.dealt[me]*.87*DAMAGE_REWARD;
 score+=(position(s,me,target)-position(root,me,target)*.25)*POSITION_WEIGHT;
 double danger=deathRisk(s,me);score-=stockval*danger;
 if(a.out)score-=200;
 for(int i=0;i<s.n;i++)if(i!=me){const F&b=s.f[i];const F&br=root.f[i];int lost=br.stocks-b.stocks;if(lost>0)score+=lost*12+(b.out?20:0);
 if(live(b)){double dr=deathRisk(s,i);if(b.last==me&&s.tick-b.lasttick<c.credit)score+=dr*(KILL_VALUE*.93);else score+=dr*3;
 if((s.tags[me]>>i&1)&&br.last!=me)score+=TAG_VALUE;
 if(b.stun>0&&b.last==me)score+=std::min(20,b.stun)*.15;
 }}
 if(a.stun>0)score-=a.stun*.18;
 if(a.act==COUNTER&&a.phase==RECOVER)score-=.8;
#ifdef WIN_BONUS
 int left=0;for(int i=0;i<s.n;i++)left+=!s.f[i].out;if(left==1&&!a.out)score+=100;
#endif
 return score;
 }
 std::vector<Candidate> candidates(const State&s)const{
 std::vector<Candidate> v;const F&a=s.f[me];auto add=[&](Order o,int hold=0,int steer=0){for(auto&k:v)if(k.o.skill==o.skill&&k.hold==hold&&k.o.move==o.move&&k.o.jump==o.jump&&k.o.drop==o.drop&&std::abs(k.o.dx-o.dx)<.04&&std::abs(k.o.dy-o.dy)<.04)return;v.push_back({o,hold,steer,0});};
 Order base=policy(s,me,0,true);add(base);add(recovery(s,me));
 int t=nearest(s,me,true);double dx=t>=0?s.f[t].x-a.x:a.face,dy=t>=0?s.f[t].y-a.y:0;double side=sign(dx);
 for(int mv=-1;mv<=1;mv++){add(order(0,dx,dy,mv));if(a.ground||a.air>0)add(order(0,dx,dy,mv,true));if(a.y>0)add(order(0,dx,dy,mv,false,true));}
#ifdef LATE_CENTER
 if(s.tick>3200&&a.stun==0&&(a.act==NONE||a.act==CHARGE)){
 auto futureX=[&](double mv){State p=s;Order o=order(0,dx,dy,mv);for(int k=0;k<3;k++){p.control(me,o,k==0,c);p.motion(me,c);F&f=p.f[me];f.x=std::round(f.x*10000)/10000;f.y=std::round(f.y*10000)/10000;f.vx=std::round(f.vx*10000)/10000;f.vy=std::round(f.vy*10000)/10000;}return p.f[me].x;};
 double lo=-1,hi=1;for(int k=0;k<20;k++){double mid=(lo+hi)/2;if(futureX(mid)>0)hi=mid;else lo=mid;}add(order(0,dx,dy,(lo+hi)/2));
 }
#endif
 if(a.stun>3||a.pull>3||(a.act!=0&&a.act!=GUARD&&a.act!=CHARGE))return v;
 if(a.act==CHARGE&&a.phase!=CHARGING)return v;
 add(order(GUARD,dx,dy,0));if(a.meter>=.3)add(order(COUNTER,dx,dy,0));
 if(a.meter>=.2){for(double bx:{-1.,0.,1.})for(double by:{-.5,0.,.7,1.})if(bx||by){if(a.y<1&&by<0)continue;add(order(BLINK,bx,by,-sign(a.x)));}}
 for(int j=0;j<s.n;j++){const F&b=s.f[j];if(j==me||!live(b)||b.invuln>15)continue;double px=b.x-a.x,py=b.y-a.y,d=std::sqrt(px*px+py*py);if(d>24)continue;double sx=sign(px);
 double ax=px+b.vx*(b.stun?4:2)-a.vx*(a.ground?.5:3);double ay=py+(b.ground?0:b.vy*3-.15)-(a.ground?0:a.vy*3-.15);
 std::vector<std::array<double,2>> dirs={{ax,ay},{sx,0},{sx,.6},{sx,-.65},{0,1},{0,-1}};
#ifdef FULL_DIRECTIONS
 if(d<5.0)for(double xx:{-1.,1.})for(double yy:{-1.8,-.65,0.,.6,1.8})dirs.push_back({xx,yy});
#endif
 if(d<6.5){for(auto &dr:dirs){add(order(ATTACK,dr[0],dr[1],side));if(a.meter>=.3)add(order(GRAB,dr[0],dr[1],side));if(d<5.8){for(int hold:{0,9,24}){add(order(CHARGE,dr[0],dr[1],side),hold);if(a.act==CHARGE)add(order(0,dr[0],dr[1],side),0);}}}
 if(a.meter>=.25)add(order(UPPER,0,1,side));if(!a.ground&&a.meter>=.2)add(order(SLAM,0,-1,side));
#ifdef JUMP_SLAM
 if(a.ground&&a.meter>=.2)for(int mv=-1;mv<=1;mv++){add(order(SLAM,0,-1,mv,true));if(a.y>0)add(order(SLAM,0,-1,mv,true,true));}
#endif
 }
 if(d<12&&a.meter>=.4&&a.cd==0){add(order(HOOK,px+b.vx*5,py+b.vy*5,side));add(order(HOOK,px,py,side));}
 if(d>4&&a.meter>=.6){add(order(WAVE,px+b.vx*6,py+b.vy*6,side));add(order(WAVE,px,py,side));}
 if(d>3&&d<10&&(a.ground||a.air)){add(order(CHARGE,px,py,side,true),12);}
 }
 if(a.act==CHARGE){for(int hold:{0,3,9,18,36}){add(order(CHARGE,dx,dy,side),hold);add(order(CHARGE,dx,dy,0),hold);}}
 return v;
 }
 double rollout(const State&root,const Candidate&can,int model,int horizon,int forced=-1,const Order&force=Order{})const{
 State s=root;double discounted=0,lastValue=0,discount=1;std::array<Order,4> orders;int started=-1;bool mainDone=false;int initialAct=s.f[me].act;
 for(int tick=0;tick<horizon;tick++){
 if(tick%3==0){for(int i=0;i<s.n;i++)orders[i]=policy(s,i,model,i==me);
 if(tick==0){orders[me]=can.o;if(forced>=0)orders[forced]=force;}
 else if(tick<6&&can.o.skill==0){orders[me]=can.o;orders[me].jump=false;}
 if(forced>=0&&tick>0&&tick<6&&force.skill==0){orders[forced]=force;orders[forced].jump=false;}
 if(forced>=0&&tick>0&&tick<9&&force.skill==GUARD&&s.f[forced].act==GUARD)orders[forced]=force;
 if(can.o.skill==CHARGE){if(s.f[me].phase==CHARGING&&s.f[me].act==CHARGE&&!mainDone){orders[me]=can.o;if(tick>0)orders[me].jump=false;if(tick>=can.hold+3){orders[me].skill=0;mainDone=true;}}else if(tick>3&&s.f[me].act!=CHARGE)mainDone=true;}
 if(can.o.skill==GUARD&&tick<9&&s.f[me].act==GUARD){orders[me]=can.o;orders[me].jump=false;}
 if(tick>0&&can.o.skill!=0&&can.o.skill!=CHARGE&&can.o.skill!=GUARD&&s.f[me].act==can.o.skill){orders[me].move=can.o.move;orders[me].drop=can.o.drop;}
 }
 s.step(orders,tick%3==0,c);
#ifdef TIME_DISCOUNT
 if(tick%3==2||s.f[me].out){double ev=evaluate(s,root);discounted+=(ev-lastValue)*discount;lastValue=ev;discount*=TIME_DISCOUNT;}
#endif
 if(s.f[me].out)break;
 }
 #ifdef TIME_DISCOUNT
 return discounted;
#else
 return evaluate(s,root);
#endif
 }
 std::vector<Order> enemyOptions(const State&s,int e)const{
 std::vector<Order> v;const F&a=s.f[e];const F&b=s.f[me];double dx=b.x-a.x,dy=b.y-a.y,side=sign(dx);
 auto add=[&](Order o){for(auto&q:v)if(q.skill==o.skill&&q.move==o.move&&q.jump==o.jump&&std::abs(q.dx-o.dx)<.08&&std::abs(q.dy-o.dy)<.08)return;v.push_back(o);};
 for(int m=0;m<3;m++)add(policy(s,e,m));
 if(a.stun>2||a.pull>2||a.gap>2||(a.act&&a.act!=CHARGE&&a.act!=GUARD))return v;
 if(a.act==CHARGE&&a.phase!=CHARGING)return v;
 for(int mv=-1;mv<=1;mv++){add(order(0,dx,dy,mv));if(a.ground||a.air)add(order(0,dx,dy,mv,true));}
 add(order(ATTACK,dx+b.vx*3,dy+b.vy*3,side));add(order(ATTACK,side,-.6,side));
 if(a.meter>=.3){add(order(GRAB,dx+b.vx*4,dy+b.vy*4,side));add(order(COUNTER,dx,dy,0));}
 add(order(GUARD,dx,dy,0));add(order(CHARGE,dx,dy,side));
 if(a.meter>=.2){add(order(BLINK,-side,.1,-side));add(order(BLINK,side,.1,side));add(order(BLINK,0,1,0));if(!a.ground)add(order(SLAM,0,-1,side));}
 if(a.meter>=.25)add(order(UPPER,0,1,side));
#ifdef TACTICAL_V2
 if(a.ground&&a.meter>=.2){add(order(SLAM,0,-1,side,true));add(order(SLAM,0,-1,0,true));}
#endif
 if(a.meter>=.4&&a.cd==0)add(order(HOOK,dx,dy,0));
 return v;
 }
 bool recoverPlan(const State&root,Order&answer)const{
 const F&r=root.f[me];if(r.stun>0||r.pull>0||!needsRecovery(root,me))return false;
 int t=nearest(root,me);if(t>=0&&r.y>-5.5&&sq(r.x-root.f[t].x)+sq(r.y-root.f[t].y)<36)return false;
 struct Node{State s;Order first;double score;};
 auto value=[&](const State&s){const F&a=s.f[me];if(a.stocks<r.stocks)return -10000.;double d;
 if(a.y<-5.5&&std::abs(a.x)<13.6)d=13.7-std::abs(a.x)+std::max(0.,-a.y)*.8+6;
 else d=std::max(0.,std::abs(a.x)-12.3)+std::max(0.,-a.y)*.8;
 double v=-d*8+a.meter*5+a.air*4+std::min(3.,a.y)*.3-s.received[me]*3;
 v-=std::max(0.,std::abs(a.x)-s.bx(c)+2)*15;v-=std::max(0.,a.y-s.bt(c)+2)*15;
 if(a.y<-11)v-=(-a.y-11)*10;if(a.vy<0&&a.y<0)v+=a.vy*2;
 return v;};
 std::vector<Node> beam{{root,Order{},value(root)}};
 for(int depth=0;depth<14;depth++){
 if(depth>2&&elapsed()>12)return false;
 std::vector<Node> next;double goal=-1e20;Order winner;
 for(auto&node:beam){
#ifdef BUDGET_SAFE
 if(depth>0&&elapsed()>8)return false;
#endif
 const F&a=node.s.f[me];if(a.stocks<r.stocks)continue;
 std::vector<Order> actions;actions.push_back(recovery(node.s,me));
 bool mobile=a.act==NONE||a.act==CHARGE;
 if(mobile){for(int mv=-1;mv<=1;mv++){actions.push_back(order(0,mv?mv:a.face,0,mv));if(a.air||a.ground)actions.push_back(order(0,mv?mv:a.face,0,mv,true));}
 if(a.gap<=2&&a.meter>=.2){for(double dx:{-1.,0.,1.})for(double dy:{0.,.5,1.})if(dx||dy)actions.push_back(order(BLINK,dx,dy,-sign(a.x)));if(a.meter>=.25)actions.push_back(order(UPPER,0,1,-sign(a.x)));}}
 for(auto&o:actions){Node p=node;p.first=depth==0?o:node.first;std::array<Order,4> os;
 for(int k=0;k<6;k++){if(k%3==0){for(int j=0;j<p.s.n;j++)os[j]=policy(p.s,j,0);os[me]=o;if(k>0)os[me].jump=false;}p.s.step(os,k%3==0,c);}
 const F&b=p.s.f[me];p.score=value(p.s);
 if(b.stocks==r.stocks&&b.ground&&b.y>=0&&std::abs(b.x)<13.3){double q=1000+b.meter*8+b.air*2-p.s.received[me]*4-std::abs(b.x)*.1;if(q>goal){goal=q;winner=p.first;}}
 next.push_back(std::move(p));}
 }
 if(goal>-1e10){answer=winner;return true;}
 if(next.empty())return false;
 std::stable_sort(next.begin(),next.end(),[](const Node&a,const Node&b){return a.score>b.score;});
 beam.clear();
 for(auto&node:next){if(node.score<-9000)break;bool dup=false;const F&a=node.s.f[me];for(auto&p:beam){const F&b=p.s.f[me];if(std::abs(a.x-b.x)<.15&&std::abs(a.y-b.y)<.15&&std::abs(a.vx-b.vx)<.06&&std::abs(a.vy-b.vy)<.06&&a.act==b.act&&a.air==b.air&&std::abs(a.meter-b.meter)<.05){dup=true;break;}}if(!dup)beam.push_back(std::move(node));if(beam.size()>=10)break;}
 }
 return false;
 }
 Order choose(State&s){
#ifdef RECOVERY_BEAM
 Order rec;if(recoverPlan(s,rec))return rec;
#endif
 target=nearest(s,me,true);auto list=candidates(s);if(list.empty())return Order{};
 int shortH=SEARCH_SHORT,longH=SEARCH_HORIZON,keepN=SEARCH_KEEP;
#ifdef ADAPT_LATE
 if(s.tick>3500){shortH=15;longH=42;keepN=14;}
#endif
 // Two different reactive opponents screen actions before deeper, robust evaluation.
 for(size_t kk=0;kk<list.size();kk++){
#ifdef BUDGET_SAFE
 if(kk>=12&&elapsed()>12){list.resize(kk);break;}
#endif
 auto&ca=list[kk];double x=rollout(s,ca,0,shortH);double y=rollout(s,ca,1,shortH);ca.cheap=.65*x+.35*y;}
 std::stable_sort(list.begin(),list.end(),[](const Candidate&a,const Candidate&b){return a.cheap>b.cheap;});
#ifdef DIVERSE_SEARCH
 if(list.size()>20){
 std::vector<Candidate> selected;std::array<int,14> used{};
 auto same=[](const Candidate&a,const Candidate&b){return a.o.skill==b.o.skill&&a.o.move==b.o.move&&a.o.jump==b.o.jump&&a.o.drop==b.o.drop&&std::abs(a.o.dx-b.o.dx)<.04&&std::abs(a.o.dy-b.o.dy)<.04;};
 for(auto&ca:list){int key=ca.o.skill;if(key==NONE&&ca.o.jump)key=11;if(key==NONE&&ca.o.drop)key=12;int cap=(key==CHARGE||key==BLINK)?4:3;if(used[key]>=cap)continue;bool duplicate=false;for(auto&q:selected)if(same(ca,q))duplicate=true;if(duplicate)continue;selected.push_back(ca);used[key]++;if(selected.size()>=28)break;}
 list=std::move(selected);
 }
#endif
 int count=std::min(keepN,int(list.size()));double best=-1e30;Order ans=list[0].o;
#ifdef DIVERSE_SEARCH
 count=list.size();
#endif
 for(int k=0;k<count;k++){if(k>3&&elapsed()>
#ifdef BUDGET_SAFE
 20
#else
 25
#endif
 )break;Candidate&ca=list[k];double sum=0,mn=1e30;for(int model=0;model<MODEL_COUNT;model++){double val=rollout(s,ca,model,longH);sum+=val;mn=std::min(mn,val);}double val=sum/MODEL_COUNT*.75+mn*.25;val+=ca.cheap*.07;
 // Tiny tie break keeps resource-free movement preferable to empty specials.
 val-=c.m[ca.o.skill].cost*.8;if(ca.o.skill==BLINK)val-=BLINK_PENALTY;ca.deep=val;
 if(val>best){best=val;ans=ca.o;}}
 
#ifdef TACTICAL_GAME
 int enemy=nearest(s,me);const F&a=s.f[me];
 if(enemy>=0&&live(s.f[enemy])&&s.f[enemy].invuln<5&&a.invuln<5&&sq(a.x-s.f[enemy].x)+sq(a.y-s.f[enemy].y)<30&&a.stun<=2&&a.pull<=2&&a.gap<=2&&(a.act==0||a.act==CHARGE||a.act==GUARD)){
 std::stable_sort(list.begin(),list.begin()+count,[](const Candidate&a,const Candidate&b){return a.deep>b.deep;});
 auto ev=enemyOptions(s,enemy);
 if(ev.size()>4&&elapsed()<15){
 // Retain action diversity; the predictable best response alone loses rock-paper-scissors fights.
 std::vector<Candidate> rows;
 for(int k=0;k<std::min(12,int(list.size()));k++)rows.push_back(list[k]);
 for(int sk:{NONE,ATTACK,GRAB,GUARD,COUNTER,CHARGE,BLINK,UPPER,SLAM}){bool exists=false;for(auto&r:rows)if(r.o.skill==sk)exists=true;if(!exists)for(auto&r:list)if(r.o.skill==sk){rows.push_back(r);break;}}
 int nr=rows.size(),nc=ev.size();std::vector<std::vector<double>> mat(nr,std::vector<double>(nc));
 for(int i=0;i<nr;i++){
#ifdef TACTICAL_V2
 if(elapsed()>24)return ans;
 double prior=(rollout(s,rows[i],0,24)+rollout(s,rows[i],1,24)+rollout(s,rows[i],2,24))/3;
 for(int j=0;j<nc;j++){double val=rollout(s,rows[i],0,24,enemy,ev[j]);mat[i][j]=.7*val+.3*prior;}
#else
 double prior=(rollout(s,rows[i],0,42)+rollout(s,rows[i],1,42)+rollout(s,rows[i],2,42))/3;for(int j=0;j<nc;j++){double val=rollout(s,rows[i],0,42,enemy,ev[j]);mat[i][j]=.3*val+.7*prior;}
#endif
 }
 std::vector<double> rv(nr),cv(nc);std::vector<int> freq(nr);int ri=0,cj=0;
 for(int it=0;it<1200;it++){ri=std::max_element(rv.begin(),rv.end())-rv.begin();freq[ri]++;for(int j=0;j<nc;j++)cv[j]+=mat[ri][j];cj=std::min_element(cv.begin(),cv.end())-cv.begin();for(int i=0;i<nr;i++)rv[i]+=mat[i][cj];}
 unsigned long long rng=(unsigned long long)(turn*0x9e3779b97f4a7c15ULL+me*0xBF58476D1CE4E5B9ULL+1234567);rng^=rng>>30;rng*=0xbf58476d1ce4e5b9ULL;rng^=rng>>27;rng*=0x94d049bb133111ebULL;rng^=rng>>31;int pick=rng%1200;for(int i=0;i<nr;i++){pick-=freq[i];if(pick<0){ans=rows[i].o;break;}}
 }
 }
#endif
 return ans;
 }
 std::pair<J,J> step(const J&obs,J&memory){begin=std::chrono::steady_clock::now();if(!initialized){c.load(obs.at("rules"));initialized=true;}me=obs.at("seat").get<int>();turn=obs.at("turn").get<int>();State s=parse(obs,c);nowtick=s.tick;restore(s,memory);Order action;if(live(s.f[me]))action=choose(s);
 prev=s;hasprev=true;prevorder=action;
 // Record the next own command's hidden drop state for the following observation.
#ifndef INVERSE_STATE
 if(action.drop&&s.f[me].ground)prev.f[me].drop=10;
#endif
 J h=J::array{};for(int i=0;i<s.n;i++){auto&a=prev.f[i];h.push_back(J::array{a.last,a.lasttick,a.act,a.mask,a.power,a.pulled,a.pull,a.reel,a.hx,a.hy,a.drop,a.slamLand});}
 memory=J{{"h",h},{"t",s.tick},{"target",target}};
 return {json(action),memory};
 }
};
}
