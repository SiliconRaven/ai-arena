#include "include/step_worker.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <limits>
#include <string>
using J=mini_json::Json;
using namespace std;

struct V {
    double x=0,y=0;
    V(){} V(double a,double b):x(a),y(b){}
    V operator+(V b)const{return {x+b.x,y+b.y};}
    V operator-(V b)const{return {x-b.x,y-b.y};}
    V operator*(double a)const{return {x*a,y*a};}
    V operator/(double a)const{return {x/a,y/a};}
    V& operator+=(V b){x+=b.x;y+=b.y;return *this;}
    V& operator-=(V b){x-=b.x;y-=b.y;return *this;}
};
static inline double dot(V a,V b){return a.x*b.x+a.y*b.y;}
static inline double norm2(V a){return dot(a,a);}
static inline double norm(V a){return sqrt(norm2(a));}
static inline V unit(V a){double d=norm(a);return d>1e-10?a/d:V();}
static inline double clampd(double x,double a,double b){return max(a,min(b,x));}
static inline double cheb(V v){return max(abs(v.x),abs(v.y));}
static inline double val(const J& j,const char* s){return j[s].as_double();}
static inline int ival(const J& j,const char* s){return (int)j[s].as_int();}
static inline V readv(const J& j,const char* x="x",const char* y="y"){return V(val(j,x),val(j,y));}
static inline J jvec(V a){return J::array{a.x,a.y};}

struct P {
    V p,v,aim;
    double bar=1, q=0;
    int skill=0,pause=0,held=0,act=0,actt=0,acd=0,hcd=0,bcd=0,last=-1,lastt=-9999,kills=0;
    bool out=false;
};
struct B {V p,v; int fuse=0,owner=0; bool alive=true;};
struct W {V a,b; bool spring=false,active=true; int regen=0;};
struct A {V move,dir; int charge=0,release=0; bool jab=false,bomb=false; V bombdir; int fuse=24;};
struct Game {
    P p[8]; B b[48]; W w[48];
    int n=0,nb=0,nw=0,you=0,tick=0;
    double half=16.5;
    double dealt=0,received=0;
};
static inline int skill(const J& x){if(!x.is_string())return 0; return x.as_string()=="hammer"?2:1;}
static inline int act(const J& x){if(!x.is_string())return 0;auto s=x.as_string();return s=="recover"?1:s=="jab"?2:3;}
static double arenaHalf(int tick){
    if(tick<145)return 16.5;
    int k=(tick-145)/150, st=145+k*150;
    double from=max(0.5,16.5-2.5*k),to=max(0.5,from-2.5);
    return from+(to-from)*min(1.0,(tick-st+1)/60.0);
}
static V nearest(V p,const W&w){V v=w.b-w.a;double l=norm2(v);double t=l?clampd(dot(p-w.a,v)/l,0,1):0;return w.a+v*t;}
static Game parse(const J&o){
    Game g;g.tick=ival(o,"tick");g.half=val(o["arena"],"half");g.n=ival(o,"n");g.you=ival(o,"you");
    for(int i=0;i<g.n;i++){
        const J& j=o["players"][i];P& p=g.p[i];p.p=readv(j);p.v=readv(j,"vx","vy");p.bar=val(j,"bar");p.q=val(j,"charge");p.skill=skill(j["skill"]);p.pause=ival(j,"pause");p.held=ival(j,"held");p.out=j["out"].as_bool();p.act=act(j["act"]);p.actt=ival(j,"act_ticks");p.acd=ival(j,"attack_cd");p.hcd=ival(j,"hammer_cd");p.bcd=ival(j,"bomb_cd");
        if(j["aim"].is_array())p.aim=V(j["aim"][0].as_double(),j["aim"][1].as_double());
        const J& lh=o["last_hit"][i];if(lh.is_array()){p.last=(int)lh[0].as_int();p.lastt=(int)lh[1].as_int();}
        p.kills=(int)o["kills"][i].as_int();
    }
    for(auto& j:o["bombs"].as_array())if(g.nb<48){B& b=g.b[g.nb++];b.p=readv(j);b.v=readv(j,"vx","vy");b.fuse=ival(j,"fuse");b.owner=ival(j,"owner");}
    for(auto& j:o["walls"].as_array())if(g.nw<48){W& w=g.w[g.nw++];w.a=readv(j,"x1","y1");w.b=readv(j,"x2","y2");w.spring=j["kind"].as_string()=="spring";}
    for(auto& j:o["broken"].as_array())if(g.nw<48){W& w=g.w[g.nw++];w.a=readv(j,"x1","y1");w.b=readv(j,"x2","y2");w.spring=false;w.active=false;w.regen=ival(j,"at");}
    return g;
}
static inline void hit(Game& g,int i,int j,double power,V u){
    g.p[j].v+=u*power;g.p[j].last=i;g.p[j].lastt=g.tick;
    if(i==g.you&&j!=g.you){
        V pos=g.p[j].p; double out=dot(u,unit(pos));
        g.dealt+=0.7+0.7*max(0.0,out)*min(1.0,cheb(pos)/max(2.,g.half));
    } else if(j==g.you)g.received+=1.0;
}
static void swing(Game&g,int i,V aim,bool hammer,double q){
    P& a=g.p[i];double reach=hammer?3.2:2.8,power=hammer?0.9*q:0.35;
    for(int j=0;j<g.n;j++)if(i!=j&&!g.p[j].out){V d=g.p[j].p-a.p;double l=norm(d);V u=l>1e-9?d/l:aim;if(l<=reach&&dot(u,aim)>=.5)hit(g,i,j,power,u);}
    for(int k=0;k<g.nb;k++)if(g.b[k].alive){V d=g.b[k].p-a.p;double l=norm(d);V u=l>1e-9?d/l:aim;if(l<=reach&&dot(u,aim)>=.5){g.b[k].v+=u*(power*2);g.b[k].owner=i;}}
    a.v-=aim*((hammer?.15:.05)*(hammer?q:1.0));
}
static void explode(Game&g,B&b){
    for(int i=0;i<g.n;i++)if(!g.p[i].out){V d=g.p[i].p-b.p;double l=norm(d);if(l<=3.5){V u=l>1e-9?d/l:V(1,0);hit(g,b.owner,i,1.4*(1-l/3.5),u);}}
    for(int i=0;i<g.nb;i++)if(g.b[i].alive&& &g.b[i]!=&b){V d=g.b[i].p-b.p;double l=norm(d);if(l<=3.5){V u=l>1e-9?d/l:V(1,0);g.b[i].v+=u*(2.8*(1-l/3.5));}}
    for(int k=0;k<g.nw;k++){W&w=g.w[k];if(w.active&&!w.spring&&norm2(nearest(b.p,w)-b.p)<=3.5*3.5){w.active=false;w.regen=g.tick+300;}}
}
static double contact(V&ap,V&av,V&bp,V&bv,double reach,double ma,double mb){
    V d=bp-ap;double dd=norm2(d);if(dd>=reach*reach)return 0;double l=sqrt(dd);V u=l>1e-9?d/l:V(1,0);double share=mb/(ma+mb);ap-=u*((reach-l)*share);bp+=u*((reach-l)*(1-share));double approach=dot(av-bv,u);if(approach<=0)return 0;double impulse=1.9*approach/(1/ma+1/mb);av-=u*(impulse/ma);bv+=u*(impulse/mb);return impulse;
}
static void bounce(V&p,V&v,double rad,const W&w,bool regen=false){
    if(p.x<min(w.a.x,w.b.x)-rad-.25||p.x>max(w.a.x,w.b.x)+rad+.25||p.y<min(w.a.y,w.b.y)-rad-.25||p.y>max(w.a.y,w.b.y)+rad+.25)return;
    V q=nearest(p,w),d=p-q;double r=rad+.25,dd=norm2(d);if(dd>=r*r)return;V u;if(dd>1e-12)u=d/sqrt(dd);else{V line=w.b-w.a;u=unit(V(-line.y,line.x));if(norm2(u)<.1)u=V(1,0);}
    p=q+u*r;double vn=dot(v,u);if(regen)v+=u*(.5-vn);else if(vn<0){double speed=w.spring?min(.5,-1.5*vn):-.8*vn;v+=u*(speed-vn);}
}
static void simtick(Game&g,const A orders[8],bool first){
    ++g.tick;g.half=arenaHalf(g.tick);
    for(int i=0;i<g.nw;i++){W&w=g.w[i];if(max(cheb(w.a),cheb(w.b))>g.half){w.active=false;w.regen=0;continue;}if(!w.active&&w.regen>0&&g.tick>=w.regen){w.active=true;w.regen=0;for(int j=0;j<g.n;j++)if(!g.p[j].out)bounce(g.p[j].p,g.p[j].v,1,w,true);for(int j=0;j<g.nb;j++)if(g.b[j].alive)bounce(g.b[j].p,g.b[j].v,.6,w,true);}}
    for(int k=0;k<g.nb;k++)if(g.b[k].alive&&--g.b[k].fuse<=0){g.b[k].alive=false;explode(g,g.b[k]);}
    for(int i=0;i<g.n;i++)if(!g.p[i].out){
        P&p=g.p[i];const A&o=orders[i];p.acd=max(0,p.acd-1);p.hcd=max(0,p.hcd-1);bool busy=p.act!=0;
        if(busy){p.actt--;if(p.actt<=0){if(p.act==2){swing(g,i,p.aim,false,1);p.act=3;p.actt=6;}else{p.act=0;p.actt=0;}}}
        int want=o.charge;if(want==2&&p.hcd>0)want=0;
        bool release=!busy&&first&&p.skill&&p.q>0&&o.release==p.skill;
        if(!busy&&!release&&want&&p.skill&&p.skill!=want){p.q=0;p.skill=0;}
        bool jab=first&&!busy&&o.jab&&p.acd==0&&!(release&&p.skill==2);
        bool charging=!busy&&!release&&!jab&&want&&p.bar>1e-8;
        if(p.pause>0)p.pause--;else if(!charging)p.bar=max(p.bar,min(1-p.q,p.bar+.02));
        if(release){double q=p.q;int sk=p.skill;p.q=0;p.skill=0;p.pause=30;if(sk==1)p.v+=o.dir*(.9*q);else{swing(g,i,o.dir,true,q);p.hcd=60;p.act=1;p.actt=5;}}
        if(jab){p.act=2;p.actt=9;p.aim=o.dir;p.acd=15;}
        if(charging){double a=min(.04,p.bar);p.bar-=a;p.q+=a;p.skill=want;}
        if(p.q<=1e-9||charging)p.held=0;else if(++p.held>=45){p.q=0;p.skill=0;p.held=0;p.pause=30;}
        if(!busy&&p.act==0&&norm2(o.move)>.01)p.v+=o.move*(.018*(p.skill==1?.2:p.skill==2?.3:1));
        if(p.bcd>0)p.bcd--;else if(first&&o.bomb&&!busy&&p.act==0&&g.nb<48){B&b=g.b[g.nb++];b.p=p.p+o.bombdir*1.8;b.v=p.v+o.bombdir*.5;b.owner=i;b.fuse=o.fuse;b.alive=true;p.bcd=90;}
    }
    for(int i=0;i<g.n;i++)if(!g.p[i].out)g.p[i].p+=g.p[i].v;
    for(int k=0;k<g.nb;k++)if(g.b[k].alive)g.b[k].p+=g.b[k].v;
    for(int i=0;i<g.n;i++)if(!g.p[i].out)for(int j=i+1;j<g.n;j++)if(!g.p[j].out){double imp=contact(g.p[i].p,g.p[i].v,g.p[j].p,g.p[j].v,2,1,1);if(imp>=.03){g.p[i].last=j;g.p[i].lastt=g.tick;g.p[j].last=i;g.p[j].lastt=g.tick;}}
    for(int i=0;i<g.n;i++)if(!g.p[i].out)for(int j=0;j<g.nb;j++)if(g.b[j].alive){P&p=g.p[i];B&b=g.b[j];V d=b.p-p.p;bool hitter=dot(p.v+b.v,d)>0;if(contact(p.p,p.v,b.p,b.v,1.6,1,.5)>=.03&&hitter)b.owner=i;}
    for(int i=0;i<g.nb;i++)if(g.b[i].alive)for(int j=i+1;j<g.nb;j++)if(g.b[j].alive)contact(g.b[i].p,g.b[i].v,g.b[j].p,g.b[j].v,1.2,.5,.5);
    for(int i=0;i<g.n;i++)if(!g.p[i].out){P&p=g.p[i];for(int k=0;k<g.nw;k++)if(g.w[k].active)bounce(p.p,p.v,1,g.w[k]);p.v=p.v*.94;if(norm2(p.v)<.000001)p.v=V();if(cheb(p.p)>g.half){p.out=true;if(p.last>=0&&p.last!=i&&g.tick-p.lastt<=90)g.p[p.last].kills++;}}
    for(int i=0;i<g.nb;i++)if(g.b[i].alive){B&b=g.b[i];for(int k=0;k<g.nw;k++)if(g.w[k].active)bounce(b.p,b.v,.6,g.w[k]);b.v=b.v*.95;if(cheb(b.p)>g.half)b.alive=false;}
}
static double brake(double v,double accel){
    double d=0;for(int i=0;i<60&&v>0;i++){v=max(0.0,v-accel);d+=v;v*=.94;}return d;
}
static double hazard(const Game&g,int id){
    const P&p=g.p[id];double factor=p.skill==1?.2:p.skill==2?.3:1.0;
    double ax=abs(p.p.x)+brake(max(0.,p.v.x*(p.p.x>=0?1:-1)),.018*factor);
    double ay=abs(p.p.y)+brake(max(0.,p.v.y*(p.p.y>=0?1:-1)),.018*factor);
    double m=g.half-max(ax,ay);double cost=0;
    if(m<5)cost+=(5-m)*(5-m)*.55;
    if(m<1.5)cost+=(1.5-m)*(1.5-m)*6;
    return cost;
}
static double radial(const Game&g,V p){return cheb(p)/max(1.,g.half);}
static double threat(const Game&g,int id){
    const P&p=g.p[id];double ans=0;
    for(int j=0;j<g.n;j++)if(j!=id&&!g.p[j].out){const P&e=g.p[j];V d=p.p-e.p;double dist=norm(d);if(dist>9||dist<.01)continue;
        V u=d/dist;double outside=dot(u,unit(p.p));double edge=clampd(radial(g,p.p),0,1.5);double rel=dot(e.v-p.v,u);
        double danger=0;
        if(e.skill==2&&e.q>.15){double gap=max(0.,dist-3.2);danger=(1.8+5*e.q)*exp(-gap*gap/3.4);}
        if(e.skill==1&&e.q>.25){double gap=max(0.,dist-2.5);danger+=(1.3+3*e.q)*exp(-gap*gap/11.0);}
        if(e.act==2&&e.actt<9){V fut=(p.p+p.v*min(8,e.actt))-(e.p+e.v*min(8,e.actt));if(dot(unit(fut),e.aim)>.3)danger+=3.0*exp(-max(0.,norm(fut)-2.8)*max(0.,norm(fut)-2.8)/3.);}
        if(e.skill==0&&e.bar>.2&&dist<4.5)danger+=.5*(4.5-dist);
        ans+=danger*(.55+edge*(.6+1.1*max(0.,outside)))*clampd(1+rel,0.7,1.6);
        // Account for the stopping room after a possible instantaneous enemy hammer.
        if(e.skill==2&&e.q>.2&&dist<7.0){
            V v=p.v+u*(.9*e.q);double factor=p.skill==1?.2:p.skill==2?.3:1.;
            double ax=abs(p.p.x)+brake(max(0.,v.x*(p.p.x>=0?1:-1)),.018*factor);
            double ay=abs(p.p.y)+brake(max(0.,v.y*(p.p.y>=0?1:-1)),.018*factor);
            double m=g.half-max(ax,ay);double prox=exp(-max(0.,dist-3.2)*max(0.,dist-3.2)/6.0);
            ans+=min(15.,max(0.,3.0-m)*1.8)*prox;
        }
    }
    return ans;
}
static int nearestEnemy(const Game&g,int id){int k=-1;double best=1e9;for(int i=0;i<g.n;i++)if(i!=id&&!g.p[i].out){double d=norm2(g.p[i].p-g.p[id].p);if(d<best)best=d,k=i;}return k;}
static V steering(const Game&g,int id,bool cautious){
    const P&p=g.p[id];V target(0,0);int k=nearestEnemy(g,id);
    // Aim for a safe interior position while remaining on the inside of nearby enemies.
    V f=p.p*(-.24)-p.v*3.2;
    if(k>=0){const P&e=g.p[k];V d=e.p-p.p;double dist=norm(d);V u=unit(d);
        double inside=radial(g,e.p)-radial(g,p.p);
        if(p.skill==2&&p.q>.25&&inside>.03&&dist>3.0&&dist<9)f+=u*(.35*clampd((dist-3)/4,0,1));
        if(dist<5.4){double mag=(5.4-dist)*.28;if(e.skill==2&&e.q>.2)mag*=1.7;if(inside>.12&&p.skill==2&&p.q>e.q+.15)mag*=.5;f-=u*mag;}
        if(dist>8&&dist<14&&p.skill==0&&inside>.15)f+=u*.18;
    }
    for(int j=0;j<g.nb;j++)if(g.b[j].alive){const B&b=g.b[j];V future=b.p+b.v*(min(18,b.fuse)*.65);V diff=p.p-future;double d=norm(diff);if(b.fuse<28&&d<5)f+=unit(diff)*((5-d)*.6);}
    if(cheb(p.p)>g.half-5){V desired=p.p*(-1);f+=unit(desired)*(.4+(5-(g.half-cheb(p.p)))*.35);}
    if(norm2(f)<.0001)return V();return unit(f);
}
static double cross(V a,V b){return a.x*b.y-a.y*b.x;}
static double pointSeg2(V p,V a,V b){V d=b-a;double q=norm2(d);double t=q>1e-12?clampd(dot(p-a,d)/q,0,1):0;return norm2(p-(a+d*t));}
static double segmentDist2(V a,V b,V c,V d){
    V r=b-a,s=d-c;double z=cross(r,s);
    if(abs(z)>1e-12){double t=cross(c-a,s)/z,u=cross(c-a,r)/z;if(t>=0&&t<=1&&u>=0&&u<=1)return 0;}
    return min(min(pointSeg2(a,c,d),pointSeg2(b,c,d)),min(pointSeg2(c,a,b),pointSeg2(d,a,b)));
}
// Route around an obstructing wall end instead of repeatedly pressing into it.
static V route(const Game&g,int id,V wish){
    if(norm2(wish)<.01)return wish;const P&p=g.p[id];
    V goal=p.p+unit(wish)*min(8.,max(3.,norm(p.p)));
    int blocked=-1;double best=1e9;
    for(int i=0;i<g.nw;i++)if(g.w[i].active){const W&w=g.w[i];
        V q=nearest(p.p,w);double start=norm(p.p-q);
        if(start<1.30&&dot(goal-p.p,p.p-q)>=0)continue;
        if(segmentDist2(p.p,goal,w.a,w.b)>=1.30*1.30)continue;
        double progress=clampd(dot(q-p.p,unit(goal-p.p)),0,norm(goal-p.p));
        if(progress<best){best=progress;blocked=i;}
    }
    if(blocked<0)return wish;
    const W&w=g.w[blocked];V u=unit(w.b-w.a),n(-u.y,u.x);if(dot(p.p-w.a,n)<0)n=n*(-1);
    double bound=max(.1,g.half-.10),cost=1e30;V waypoint;
    for(int k=0;k<2;k++){
        V end=k?w.b:w.a,away=u*(k?1:-1);V candidate=end+away*2.0-n*1.65;
        candidate.x=clampd(candidate.x,-bound,bound);candidate.y=clampd(candidate.y,-bound,bound);
        if(norm2(candidate-p.p)<.01||pointSeg2(candidate,w.a,w.b)<1.28*1.28)continue;
        double d2=segmentDist2(p.p,candidate,w.a,w.b);if(d2<1.22*1.22)continue;
        double score=norm(candidate-p.p)+norm(goal-candidate);
        for(int j=0;j<g.nw;j++)if(j!=blocked&&g.w[j].active&&segmentDist2(p.p,candidate,g.w[j].a,g.w[j].b)<1.22*1.22)score+=8;
        if(score<cost){cost=score;waypoint=candidate;}
    }
    if(cost==1e30)return wish;
    V dir=waypoint-p.p-p.v*3.0;return norm2(dir)>.001?unit(dir):wish;
}
struct Context {V inferred[8];bool valid[8]={};};
// A hammer can accelerate a bomb to nearly twice a player's dash speed.
// Detect a possible shot that would cross the player's path in the next turn.
static V cannonAim(const Game&g,int shooter,int target){
    if(shooter==target||g.p[shooter].out||g.p[target].out)return V();
    const P&e=g.p[shooter];const P&p=g.p[target];
    if(e.act||e.skill!=2||e.q<.2)return V();
    double best=1e9;V answer;
    for(int i=0;i<g.nb;i++)if(g.b[i].alive&&g.b[i].fuse>1){
        const B&b=g.b[i];V delta=b.p-e.p;double l=norm(delta);if(l>3.2||l<1e-7)continue;V u=delta/l;
        V rel=b.v+u*(1.8*e.q)-p.v;double vv=norm2(rel);if(vv<.01)continue;
        V to=p.p-b.p;double t=dot(to,rel)/vv;if(t<0||t>8.0)continue;
        double miss=norm(to-rel*t);if(miss<2.1&&miss<best){best=miss;answer=u;}
    }
    return answer;
}
static A enemyAction(const Game&g,int i,const Context&ctx,int step,int mode){
    const P&p=g.p[i];A a;int k=nearestEnemy(g,i);if(k<0)return a;const P&e=g.p[k];V d=e.p-p.p;double dist=norm(d);V u=unit(d);V centr=unit(p.p*(-1));
    V f=steering(g,i,true);
    if(ctx.valid[i]&&step<12){V v=ctx.inferred[i];if(norm2(v)>.01)f=unit(v*.8+f*.2);}
    if(mode==1&&k==g.you&&dist<10){f=unit(u*.85+centr*.25-p.v*1.2);}
    a.move=f;
    if(mode==1){V u=cannonAim(g,i,g.you);if(norm2(u)>.1){a.release=2;a.dir=u;return a;}}
    if(p.skill==2&&p.q>.08&&dist<=3.2){a.release=2;a.dir=u;return a;}
    if(p.skill==1&&p.q>.35&&dist<7&&dist>2.05&&p.act==0){double in=radial(g,e.p)-radial(g,p.p);if(mode==1||in>-.05){a.release=1;a.dir=u;}}
    if(!a.release&&p.skill==2)a.charge=2;
    if(!a.release&&p.skill==1)a.charge=1;
    if(p.skill==0&&p.hcd<=1&&p.bar>.22&&dist<7.5&&p.act==0)a.charge=2;
    if(p.skill==0&&p.hcd>6&&p.acd<=1&&dist<3.0&&p.act==0){a.jab=true;a.dir=unit(d+(e.v-p.v)*5.0);a.charge=0;}
    return a;
}
static bool useStaging(const Game&g){return g.n<=6;}
static A autoAction(const Game&g,int i){
    const P&p=g.p[i];A a;a.move=route(g,i,steering(g,i,true));int k=nearestEnemy(g,i);if(k<0)return a;const P&e=g.p[k];double dist=norm(e.p-p.p);V u=unit(e.p-p.p);
    if(p.skill==2){if(dist<=3.2&&p.q>.12){a.release=2;a.dir=u;}else if(!useStaging(g)||p.q<.70||p.held>=30||dist<5.5)a.charge=2;}
    else if(p.skill==1){if(hazard(g,i)>15&&p.q>.1){a.release=1;a.dir=unit(p.p*(-1)-p.v*4);}else a.charge=1;}
    else if(p.hcd<=1&&p.bar>.25&&dist<8.0&&hazard(g,i)<5&&(g.n<6||radial(g,p.p)<.43||dist<6.5))a.charge=2;
    return a;
}
static double scoreState(const Game&g,const Game&start,bool final){
    const P&p=g.p[g.you];const P&s=start.p[g.you];
    if(p.out)return -2000-(g.tick-start.tick)*-1.2;
    double score=0;int alive=0;
    for(int i=0;i<g.n;i++)if(i!=g.you){if(!g.p[i].out)alive++;else if(!start.p[i].out){score+=85;if(g.p[i].last==g.you&&g.tick-g.p[i].lastt<=90)score+=22;}}
    double h=hazard(g,g.you);score-=min(600.,h)*1.5;
    score-=threat(g,g.you)*2.0;
    double r=radial(g,p.p);
    // Plans must keep room around bombs, including imperfectly predicted bounces.
    for(int i=0;i<g.nb;i++)if(g.b[i].alive&&g.b[i].fuse<=36){
        const B&b=g.b[i];double t=min(12,b.fuse);
        V bp=b.p+b.v*(t*(1.0-.022*t));V pp=p.p+p.v*(min(6,b.fuse)*.8);
        double d=norm(bp-pp);double near=max(0.,5.0-d);
        double soon=clampd((39-b.fuse)/30.0,.12,1.0);
        score-=near*near*soon*(.45+1.7*r*r);
    }// Keeping the center gives room to absorb an unexpected hit from any direction.
    score-=28.0*r*r;
    double nextHalf=arenaHalf(g.tick+75);double nextMargin=nextHalf-cheb(p.p);
    if(nextHalf<g.half-.05&&nextMargin<3.0)score-=1.5*(3.0-nextMargin)*(3.0-nextMargin);
    if(r>.55)score-=18.0*(r-.55)*(r-.55);
    score-=norm2(p.v)*2.0;
    int k=nearestEnemy(g,g.you);
    if(k>=0){const P&e=g.p[k];double dist=norm(p.p-e.p);double in=radial(g,e.p)-r;
        // A loaded hammer is valuable when the opponent is reachable and outside us.
        if(p.skill==2){double useful=clampd((11-dist)/7,0,1);double hold=p.held>30&&(!useStaging(g)||p.bar<.03)?clampd((45-p.held)/15.,0,1):1.;score+=p.q*(2.5+5.0*useful)*hold;if(dist<5&&in>.03)score+=p.q*3*clampd(in*3,0,1);}
        if(p.skill==1)score+=p.q*0.8;
        if(dist<3.4&&p.skill!=2&&e.skill==2&&e.q>.15)score-=3;
        if(dist<8&&dist>3.0&&p.skill==2&&p.q>.2&&in>.08)score-=.20*(dist-3);
    }
    score+=p.bar*1.3;
    score+=g.dealt*2.2-g.received*2.5;
    if(final){
      for(int i=0;i<g.n;i++)if(i!=g.you&&!g.p[i].out&&!start.p[i].out){const P&e=g.p[i];if(e.last==g.you&&g.tick-e.lastt<90){double m=g.half-max(abs(e.p.x+e.v.x*5),abs(e.p.y+e.v.y*5));score+=max(0.,4-m)*.45;}}
    }
    return score;
}
static double rollout(const Game&initial,const A&mine,const Context&ctx,int mode,int horizon){
    Game g=initial;double accum=0;A actions[8];
    for(int t=0;t<horizon;t++){
        if(t%6==0){for(int i=0;i<g.n;i++)if(!g.p[i].out)actions[i]=i==g.you?(t==0?mine:autoAction(g,i)):enemyAction(g,i,ctx,t,mode);}
        simtick(g,actions,t%6==0);
        if(g.p[g.you].out)return -2000+(t*.8);
        int alive=0;for(int i=0;i<g.n;i++)alive+=!g.p[i].out;
        if(alive<=1){
            double win=0;for(int i=0;i<g.n;i++)if(i!=g.you&&!initial.p[i].out&&g.p[i].out)win+=85;
            win+=22*max(0,g.p[g.you].kills-initial.p[g.you].kills);
            return .42*(accum/max(1,(t+1)/6))+.58*win;
        }
        if(t%6==5)accum+=scoreState(g,initial,false);
    }
    int steps=horizon/6;return .42*(accum/max(1,steps))+.58*scoreState(g,initial,true);
}
static double safetyRollout(const Game&initial,const A&mine,const Context&ctx,int horizon){
    Game g=initial;A actions[8];double sum=0,last=0;int count=0;
    for(int t=0;t<horizon;t++){
        if(t%6==0)for(int i=0;i<g.n;i++)if(!g.p[i].out)actions[i]=i==g.you?(t==0?mine:autoAction(g,i)):enemyAction(g,i,ctx,t,0);
        simtick(g,actions,t%6==0);
        if(g.p[g.you].out)return -1000+.4*t;
        if(t%6==5){
            const P&p=g.p[g.you];double radius=radial(g,p.p),future=arenaHalf(g.tick+42);
            double margin=future-cheb(p.p),shortage=max(0.,3.0-margin);
            last=-min(300.,hazard(g,g.you))*1.4-16*radius*radius-1.2*shortage*shortage-.7*threat(g,g.you);
            sum+=last;++count;
        }
    }
    return .4*sum/max(1,count)+.6*last;
}
static Context infer(const J&o,J&memory,const Game&g){
    Context c;
    if(memory.is_object()&&memory["tick"].is_int()&&memory["tick"].as_int()==g.tick-6&&memory["vel"].is_array()&&memory["vel"].size()==(size_t)g.n){
        for(int i=0;i<g.n;i++){auto&v=memory["vel"][i];if(!v.is_array()||v.size()!=2)continue;V old(v[0].as_double(),v[1].as_double());V delta=g.p[i].v-old*pow(.94,6);double l=norm(delta);if(l>.009&&l<.13&&!g.p[i].out){c.inferred[i]=unit(delta);c.valid[i]=true;}}
        for(const auto&e:o["events"].as_array())if(e["type"].is_string()){
            auto ty=e["type"].as_string();if(ty=="bump"){int a=ival(e,"a"),b=ival(e,"b");if(a>=0&&a<g.n)c.valid[a]=false;if(b>=0&&b<g.n)c.valid[b]=false;}
            else if(ty=="spring"&&e.contains("seat")&&e["seat"].is_number()){int a=ival(e,"seat");if(a>=0&&a<g.n)c.valid[a]=false;}
            else if((ty=="dash"||ty=="hammer"||ty=="attack")&&e["seat"].is_number()){int a=ival(e,"seat");if(a>=0&&a<g.n)c.valid[a]=false;}
        }
    }
    J::array vs;for(int i=0;i<g.n;i++)vs.push_back(jvec(g.p[i].v));memory=J{{"tick",g.tick},{"vel",vs}};
    return c;
}
static vector<V> dirs(){vector<V>d;for(int i=0;i<16;i++){double a=i*3.14159265358979323846/8;d.emplace_back(cos(a),sin(a));}return d;}
static A choose(const Game&g,const Context&ctx){
    const P&p=g.p[g.you];if(p.out)return A();vector<V> ds=dirs();ds.push_back(V());V st=steering(g,g.you,true);if(norm2(st)>.1)ds.push_back(st);V path=route(g,g.you,st);if(norm2(path)>.1)ds.push_back(path);
    vector<A> opts;
    int nearest=nearestEnemy(g,g.you); double nd=nearest>=0?norm(g.p[nearest].p-p.p):100.;
    bool bombShot=false;for(int i=0;i<g.n;i++)if(i!=g.you&&norm2(cannonAim(g,i,g.you))>.1)bombShot=true;
    for(V v:ds){A a;a.move=v;opts.push_back(a);if(p.act==0){if((p.skill==2||(p.skill==0&&p.bar>.16&&(radial(g,p.p)<(g.n>=6?.43:.60)||nd<(g.n>=6?6.5:7.0))))&&p.hcd<=1&&p.bar>.03&&!(useStaging(g)&&p.skill==2&&p.q>=.70&&p.held<30&&nd>5.5)){a.charge=2;opts.push_back(a);}if((p.skill==1||(p.skill==0&&p.hcd>24&&p.bar>.25&&nd<8))&&p.bar>.03){a.charge=1;opts.push_back(a);}}}
    if(p.act==0&&p.skill==2&&p.q>.001){
        for(int i=0;i<g.n;i++)if(i!=g.you&&!g.p[i].out&&norm2(g.p[i].p-p.p)<=3.2*3.2){A a;a.release=2;a.dir=unit(g.p[i].p-p.p);opts.push_back(a);}
        for(int i=0;i<g.n;i++)if(i!=g.you&&!g.p[i].out&&norm2(g.p[i].p-p.p)<=3.2*3.2)
            for(int j=i+1;j<g.n;j++)if(j!=g.you&&!g.p[j].out&&norm2(g.p[j].p-p.p)<=3.2*3.2){
                V v=unit(unit(g.p[i].p-p.p)+unit(g.p[j].p-p.p));if(norm2(v)>.1){A a;a.release=2;a.dir=v;opts.push_back(a);}
            }
        for(int i=0;i<g.nb;i++)if(g.b[i].alive&&g.b[i].fuse>1&&g.b[i].fuse<30&&norm2(g.b[i].p-p.p)<=3.2*3.2){A a;a.release=2;a.dir=unit(g.b[i].p-p.p);opts.push_back(a);}
        bool danger=bombShot||radial(g,p.p)>.60||hazard(g,g.you)>2.5;
        for(int i=0;i<g.n;i++)if(i!=g.you&&!g.p[i].out&&g.p[i].skill==2&&g.p[i].q>.3&&norm2(g.p[i].p-p.p)<3.2*3.2)danger=true;
        if(p.held>=39||danger){
            V v=unit(p.p+p.v*5);if(norm2(v)<.1)v=V(1,0);A a;a.release=2;a.dir=v;opts.push_back(a);
            if(danger)for(int j=0;j<8;j++){double theta=j*3.14159265358979323846/4;a.dir=V(cos(theta),sin(theta));opts.push_back(a);}
        }
    }
    if(p.act==0&&p.skill==1&&p.q>.001)for(V v:ds)if(norm2(v)>.1){A a;a.release=1;a.dir=v;a.move=unit(p.p*(-1)-p.v*3);opts.push_back(a);}
    if(p.act==0&&p.acd<=1&&p.skill!=2&&p.hcd>1)for(int i=0;i<g.n;i++)if(i!=g.you&&!g.p[i].out){V d=g.p[i].p-p.p;double dist=norm(d);V predicted=d+(g.p[i].v-p.v)*6.;if(dist<4.3&&norm(predicted)<3.3){A a;a.jab=true;a.dir=unit(predicted);opts.push_back(a);}}
    double best=-1e30;A chosen;
    int until=91;for(int t=1;t<=90;t++)if(arenaHalf(g.tick+t)<g.half-.03){until=t;break;}
    bool shrinkingSoon=until<=90&&(radial(g,p.p)>.72||g.half-cheb(p.p)<2.0);
    int safeH=min(90,max(42,((until+35+5)/6)*6));vector<double> safes(opts.size(),0.);
    vector<pair<double,int>> ranked; ranked.reserve(opts.size());
    for(int i=0;i<(int)opts.size();i++){
        const A&a=opts[i];double s=rollout(g,a,ctx,0,30);
        if(shrinkingSoon){safes[i]=safetyRollout(g,a,ctx,safeH);s+=.25*safes[i];}
        if(a.jab)s-=2.0;
        ranked.push_back({s,i});
    }
    sort(ranked.begin(),ranked.end(),[](auto a,auto b){return a.first>b.first;});
    for(int k=0;k<(int)ranked.size()&&(bombShot||k<12);k++){
        int index=ranked[k].second;const A&a=opts[index];double s2=rollout(g,a,ctx,1,30)-(a.jab?2.0:0.0)+.25*safes[index];
        double s=.65*ranked[k].first+.35*s2;
        if(s>best){best=s;chosen=a;}
    }
    // Evaluate short-fuse throws with the same body physics used for movement.
    if(p.act==0&&p.bcd==0&&!chosen.jab&&chosen.release!=2&&nd<16&&arenaHalf(g.tick+42)-cheb(p.p)>3.8){
        vector<V> throws=dirs();
        for(int i=0;i<g.n;i++)if(i!=g.you&&!g.p[i].out){
            const P&e=g.p[i];V d=e.p-p.p;
            if(norm(d)<16){
                throws.push_back(unit(d));
                throws.push_back(unit(d+(e.v-p.v)*9));
                throws.push_back(unit(d+e.v*12));
            }
        }
        double base0=rollout(g,chosen,ctx,0,48),base1=rollout(g,chosen,ctx,1,48);
        double bombBest=1.4;
        for(V v:throws)if(norm2(v)>.5){
            for(int fuse:{24,30,36}){
                A a=chosen;a.bomb=true;a.bombdir=v;a.fuse=fuse;
                double s0=rollout(g,a,ctx,0,48)-base0;
                if(s0<0.3)continue;
                double s1=rollout(g,a,ctx,1,48)-base1;
                double s=.55*s0+.45*s1;
                if(min(s0,s1)<-2)s-=1.0;
                if(s>bombBest){bombBest=s;chosen=a;}
            }
        }
    }
    return chosen;
}
static J step(const J&obs,J&memory){
    Game g=parse(obs);Context ctx=infer(obs,memory,g);A a=choose(g,ctx);J out=J::object{};
    if(norm2(a.move)>.001)out["move"]=jvec(a.move);
    if(a.charge)out["charge"]=a.charge==1?"dash":"hammer";
    if(a.release)out[a.release==1?"dash":"hammer"]=jvec(a.dir);
    if(a.jab)out["attack"]=jvec(a.dir);
    if(a.bomb)out["bomb"]=J{{"dir",jvec(a.bombdir)},{"fuse",a.fuse}};
    return out;
}
int main(){return kb_step_worker::run([](const J&obs,J&memory){J order=step(obs,memory);return make_pair(order,memory);});}
