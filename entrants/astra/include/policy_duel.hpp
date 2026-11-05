#pragma once
#include "sim.hpp"
#include <chrono>
namespace policy_duel {
using namespace arena_bot;
struct Context {std::array<V,8> inferred{};std::array<double,8> confidence{};int rootTick=0;};
inline double stopping(double v,double accel){if(v<=0)return 0;double result=0;for(int k=0;k<70&&v>0;++k){v=std::max(0.,v-accel);result+=v;v*=.94;}return result;}
inline V stopped(const Player&p){double a=.018*(p.skill==1?.2:p.skill==2?.3:1);V v=p.v,dx{};int busy=p.act?std::min(15,p.actt+(p.act==2?6:0)):0;for(int k=0;k<busy;++k){dx+=v;v*=.94;}return p.p+dx+V{std::copysign(stopping(std::abs(v.x),a),v.x),std::copysign(stopping(std::abs(v.y),a),v.y)};}
inline int nearestEnemy(const World&s,int i){int target=-1;double best=1e10;for(int j=0;j<s.n;++j)if(j!=i&&!s.p[j].out){double d=(s.p[j].p-s.p[i].p).norm2();if(d<best){best=d;target=j;}}return target;}
inline V movement(const World&s,int i,int mode,const Context&ctx){const auto&a=s.p[i];int t=nearestEnemy(s,i);if(t<0)return {};const auto&q=s.p[t];V toward=q.p-a.p;double d=toward.len();V u=toward.unit();double safehalf=s.halfAt(s.tick+24);V goal{};
 bool armed=a.skill==2&&a.charge>=.24;bool vulnerable=q.skill==2&&q.charge>=.2&&q.act==0;
 if(mode==1){goal=q.p+q.v*5-u*(armed?2.5:2.2);}else if(mode==2){goal={};if(d<5.3)goal=a.p-u*(5.3-d)*2;}
 else {if(armed&&d<6.8){V radial=q.p.unit();goal=q.p-radial*2.85; if((goal-a.p).len()>d+1.6)goal=q.p-u*2.7;}else if(a.hcd>0||a.bar<.18){if(d<4.0)goal=a.p-u*(4.4-d)*2;}else if(d<4.5){goal=a.p-u*(4.5-d);}}
 goal.x=clamp(goal.x,-std::max(.1,safehalf-3.2),std::max(.1,safehalf-3.2));goal.y=clamp(goal.y,-std::max(.1,safehalf-3.2),std::max(.1,safehalf-3.2));
 V desired=(goal-a.p)*.09;double maxspeed=a.skill?.075:.24;if(desired.len()>maxspeed)desired=desired.unit()*maxspeed;
 if(mode==3&&ctx.confidence[i]>.4&&s.tick-ctx.rootTick<12){desired=ctx.inferred[i]*.24;}
 V nearfuture=a.p+a.v*5;
 // Deflect the desired velocity around capsule walls rather than pushing into them.
 for(int k=0;k<s.nw;++k){const auto&w=s.w[k];if(!w.live&&!(w.back>s.tick&&w.back-s.tick<36))continue;V off=nearfuture-w.nearest(nearfuture);double z=off.len();if(z<2.1&&z>.001){V normal=off/z;double in=dot(desired,normal);if(in<0)desired-=normal*in;desired+=normal*((2.1-z)*.10);}}
 for(int k=0;k<s.nb;++k){const auto&b=s.b[k];if(b.fuse>36)continue;double future=std::min(12,b.fuse);V bombpos=b.p+b.v*((1-std::pow(.95,future))/.05);V off=nearfuture-bombpos;double bd=off.len();if(bd<4.7&&bd>1e-5)desired+=off/bd*((4.7-bd)*.14*(b.fuse<12?1.8:1));}
 if(vulnerable&&d<4.1&&!armed){desired-=u*((4.1-d)*.12);}
 V edgefuture=stopped(a);double edge=safehalf-1.6;
 if(edgefuture.x>edge)desired.x-=.16*(edgefuture.x-edge);if(edgefuture.x<-edge)desired.x+=.16*(-edge-edgefuture.x);
 if(edgefuture.y>edge)desired.y-=.16*(edgefuture.y-edge);if(edgefuture.y<-edge)desired.y+=.16*(-edge-edgefuture.y);
 V correction=desired-a.v;return correction.len()>.012?correction.unit():V{};
}
inline Order policy(const World&s,int i,int mode,const Context&ctx){Order o;const auto&a=s.p[i];if(a.out)return o;o.move=movement(s,i,mode,ctx);if(a.act)return o;int t=nearestEnemy(s,i);if(t<0)return o;const auto&q=s.p[t];V d=q.p-a.p;double dist=d.len();V dir=d.unit();
 if(a.skill==2&&a.charge>0){int best=-1;double val=-1;V aim;for(int j=0;j<s.n;++j)if(j!=i&&!s.p[j].out&&(s.p[j].p-a.p).norm2()<=3.2*3.2){V u=(s.p[j].p-a.p).unit();double v=0;for(int k=0;k<s.n;++k)if(k!=i&&!s.p[k].out){V z=s.p[k].p-a.p;double dd=z.len();if(dd<=3.2&&dot(z,u)>=.5*dd)v+=1+std::max(0.,dot(z.unit(),s.p[k].p.unit()));}if(v>val){val=v;best=j;aim=u;}}if(best>=0&&(a.charge>=.24||s.half<5||a.held>30)){o.release=aim;return o;}
 for(int k=0;k<s.nb;++k){const auto&b=s.b[k];V off=b.p-a.p;double dd=off.len();if(b.fuse>1&&b.fuse<20&&dd<3.2&&a.charge>=.18){double risk=std::max(0.,3.7-(b.p+b.v*std::min(8,b.fuse)-a.p-a.v*std::min(8,b.fuse)).len());if(risk>1.0){o.release=off.unit();return o;}}}
 if(a.bar>.01&&a.charge<.96)o.charge=2;else if(a.held>=36){o.release=(a.p+a.v*7).unit();if(o.release.norm2()==0)o.release=-dir;}return o;}
 if(a.skill==1&&a.charge>0){V stop=stopped(a);if(stop.maxabs()>s.half-1.6){o.release=(-a.p-a.v*10).unit();return o;}if((a.charge>=.48&&dist<7.5)||(a.held>30)){V aim=(q.p+q.v*5-a.p).unit();V endpoint=a.p+(a.v+aim*(a.charge*.9))*10;if(endpoint.maxabs()<s.half-2&&dot(dir,aim)>.9)o.release=aim;else if(a.held>30)o.release=(-a.p-a.v*10).unit();}if(o.release.norm2()==0)o.charge=1;return o;}
 if(a.hcd<=6&&a.bar>=.2&&dist<(mode==1?8.5:7.0)&&stopped(a).maxabs()<s.half-2.0){o.charge=2;return o;}
 if(a.acd==0)for(int k=0;k<s.nb;++k){const auto&b=s.b[k];if(b.fuse<11||b.fuse>27)continue;V rel=b.p+b.v*7.4-a.p-a.v*7.1;if(rel.len()<2.65&&(b.p-a.p).len()<5.4){o.attack=rel.unit();return o;}}
 // Only commit the slow jab when contact at impact is reasonably likely.
 V future=d+(q.v-a.v)*6.8;
 if(a.acd==0&&future.len()<2.65&&dist<4.5&&!(q.skill==2&&q.charge>.3)&&a.p.maxabs()<s.half-2){o.attack=future.unit();return o;}
 if(mode==1&&a.hcd>18&&a.bar>.6&&dist>4.2&&dist<9)o.charge=1;
 return o;
}
inline double edgeRisk(const Player&a,double half){V stop=stopped(a);double total=0;for(double z:{std::abs(stop.x),std::abs(stop.y)}){double m=half-z;if(m<0)total+=45+60*std::min(5.,-m);else total+=18./(m+.65);}return total;}
inline double evaluate(const World&s,const World&root){int me=s.you;const auto&a=s.p[me];double value=(a.kills-root.p[me].kills)*double(s.n)*10/(s.n-1);double half=s.halfAt(s.tick+12);std::array<double,8> strength{};
 for(int i=0;i<s.n;++i)if(!s.p[i].out){const auto&p=s.p[i];double er=edgeRisk(p,half);double near=30;for(int j=0;j<s.n;++j)if(j!=i&&!s.p[j].out)near=std::min(near,(s.p[j].p-p.p).len());double ready=std::max(.15,1.-p.held/50.)*(p.act?std::max(.25,1.-p.actt/15.):1.);double energy=p.skill==2?6*p.charge*std::exp(-std::max(0.,near-3.2)/5.)*ready:p.skill==1?1.5*p.charge*ready:0;double z=-er-.06*p.p.norm2()+2*p.bar+energy;for(int j=0;j<s.n;++j)if(j!=i&&!s.p[j].out){const auto&q=s.p[j];V d=p.p-q.p;double len=d.len();if(q.skill==2&&q.charge>.1&&!q.act&&len<4.5){Player hit=p;hit.v+=d.unit()*(.9*q.charge);z-=(4.5-len)*q.charge*4+clamp((4.-len)/1.5,0,1)*std::min(100.,std::max(0.,edgeRisk(hit,half)-er))*.35;}}
 for(int k=0;k<s.nb;++k)if(s.b[k].fuse<18){const auto&b=s.b[k];V bp=b.p+b.v*((1-std::pow(.95,b.fuse))/.05);double d=(p.p+p.v*5-bp).len();if(d<3.8)z-=(3.8-d)*(18-b.fuse)*.22;}strength[i]=z;}
 for(int j=0;j<s.n;++j)if(j!=me&&!root.p[j].out){double probability=0;if(s.p[j].out){if(!a.out||s.p[j].deatht<a.deatht)probability=1;else if(s.p[j].deatht==a.deatht)probability=a.kills==s.p[j].kills?.5:(a.kills>s.p[j].kills?1:0);}else if(!a.out)probability=1/(1+std::exp(clamp((strength[j]-strength[me])/12.,-30,30)));value+=100./(s.n-1)*probability;}
 if(a.out)value-=25*std::exp(-double(std::max(0,a.deatht-root.tick))/70.);else if(s.alive()>1)value-=(root.alive()>3?.4:.07)*edgeRisk(a,half)+(root.alive()>3?.07:.01)*a.p.norm2();return value;
}
// Physics-based, bounded receding-horizon search. Use a nominal 48 ms search budget
// with headroom for communication; never access external state.
struct Search {World root;Context ctx;std::chrono::steady_clock::time_point began;int simulations=0;int adversarySeat=-1;std::vector<Order> responses;double timeBudget=48;bool expired()const{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count()>timeBudget;}
 double rollout(const Order&first,int scenario,int horizon=6){World s=root;double total=0;for(int turn=0;turn<horizon;++turn){std::array<Order,8> orders;for(int i=0;i<s.n;++i)if(!s.p[i].out){if(i==s.you)orders[i]=turn?policy(s,i,0,ctx):first;else orders[i]=policy(s,i,scenario==0?3:scenario,ctx);}s.step(orders);if(turn==0||turn==2)total+=evaluate(s,root)*.10;}++simulations;return evaluate(s,root)+total;}
 double score(const Order&o,int scenarios=3,int horizon=6){double total=0,worst=1e10;for(int k=0;k<scenarios;++k){double v=rollout(o,k,horizon);total+=v;worst=std::min(worst,v);}return root.alive()>3?.5*total/scenarios+.5*worst:.70*total/scenarios+.30*worst;}
 double knownRollout(const Order&first,int opponent,const Order&reply,int scenario=0,int horizon=7){World s=root;double total=0;for(int turn=0;turn<horizon;++turn){std::array<Order,8> orders{};for(int i=0;i<s.n;++i)if(!s.p[i].out){if(i==s.you)orders[i]=turn?policy(s,i,0,ctx):first;else if(i==opponent&&turn==0)orders[i]=reply;else orders[i]=policy(s,i,scenario==0?3:scenario,ctx);}s.step(orders);if(turn==0||turn==2)total+=evaluate(s,root)*.10;}++simulations;return evaluate(s,root)+total;}
 double robustScore(const Order&o){if(responses.empty())return score(o,3,7);double sum=0,worst=1e10;for(int k=0;k<int(responses.size());++k){double v=knownRollout(o,adversarySeat,responses[k],k%3);sum+=v;worst=std::min(worst,v);}return .68*sum/responses.size()+.32*worst;}
 void prepareResponses(const std::vector<Order>&ours){int target=nearestEnemy(root,root.you);if(target<0||root.alive()>6||(root.p[target].p-root.p[root.you].p).len()>(root.alive()>3?5.5:9))return;Search foe;foe.root=root;foe.root.you=target;foe.ctx=ctx;foe.began=began;auto options=foe.candidates();const auto&p=root.p[target];if(p.bcd==0&&!p.act){Order o=policy(root,target,0,ctx);o.release={};o.attack={};V aim=(root.p[root.you].p+root.p[root.you].v*10-p.p-p.v*10).unit();for(double delta:{-.3,0.,.3}){o.bomb=rotate(aim,delta);o.fuse=24;options.push_back(o);}o.bomb=(root.p[root.you].p-p.p).unit();options.push_back(o);o.bomb=(root.p[root.you].p+root.p[root.you].v*3-p.p-p.v*3).unit();options.push_back(o);}
 std::vector<std::pair<double,int>> ranks;for(int k=0;k<int(options.size());++k){double value=0,worst=1e10;for(const auto&mine:ours){double v=foe.knownRollout(options[k],root.you,mine,0,6);value+=v;worst=std::min(worst,v);}ranks.push_back({.8*value/ours.size()+.2*worst,k});if(expired())return;}std::sort(ranks.rbegin(),ranks.rend());adversarySeat=target;
 for(auto item:ranks){Order o=options[item.second];bool same=false;for(auto&e:responses)if(o.charge==e.charge&&o.fuse==e.fuse&&(o.move-e.move).norm2()<.12&&(o.release-e.release).norm2()<.12&&(o.attack-e.attack).norm2()<.12&&(o.bomb-e.bomb).norm2()<.12)same=true;if(!same)responses.push_back(o);if(responses.size()>=5)break;}
 responses.push_back(policy(root,target,1,ctx));responses.push_back(policy(root,target,2,ctx));
 }
 std::vector<Order> candidates(){const auto&a=root.p[root.you];std::vector<Order> out;auto add=[&](Order o){for(auto&old:out)if(o==old)return;out.push_back(o);};Order base=policy(root,root.you,0,ctx);add(base);std::vector<V>moves{base.move,{},(-a.p-a.v*12).unit(),(-a.v).unit()};for(int k=0;k<16;++k)moves.push_back(angle(k*PI/8));for(int j=0;j<root.n;++j)if(j!=root.you&&!root.p[j].out&&(root.p[j].p-a.p).len()<8){V d=(root.p[j].p-a.p).unit();moves.push_back(d);moves.push_back(-d);moves.push_back(d.perp());moves.push_back(-d.perp());}
 for(V m:moves){Order o;o.move=m;add(o);if(a.act)continue;if(a.hcd<=1&&(a.bar>.03||a.skill==2)){o.charge=2;add(o);}if((a.bar>.03||a.skill==1)&&(a.hcd>6||a.p.maxabs()>root.half-4)){o.charge=1;add(o);}}
 if(a.act)return out;
 if(a.skill==2&&a.charge>0){for(int j=0;j<root.n;++j)if(j!=root.you&&!root.p[j].out&&(root.p[j].p-a.p).len()<=3.201){Order o;o.release=(root.p[j].p-a.p).unit();add(o);for(int k=j+1;k<root.n;++k)if(k!=root.you&&!root.p[k].out&&(root.p[k].p-a.p).len()<3.201){o.release=((root.p[j].p-a.p).unit()+(root.p[k].p-a.p).unit()).unit();add(o);}}
 for(int k=0;k<root.nb;++k)if((root.b[k].p-a.p).len()<3.201){Order o;o.release=(root.b[k].p-a.p).unit();add(o);}
 if(a.held>30||stopped(a).maxabs()>root.half-3.0||root.nb>0)for(int k=0;k<16;++k){Order o;o.release=angle(k*PI/8);add(o);}}
 if(a.skill==1&&a.charge>0){std::vector<V> ds;for(int k=0;k<24;++k)ds.push_back(angle(k*PI/12));ds.push_back((-a.v).unit());ds.push_back((-a.p-a.v*12).unit());for(int j=0;j<root.n;++j)if(j!=root.you&&!root.p[j].out){ds.push_back((root.p[j].p+root.p[j].v*4-a.p).unit());ds.push_back((root.p[j].p-a.p).unit());}for(V d:ds){Order o;o.release=d;o.move=(-a.p-a.v*10-d*(a.charge*8)).unit();add(o);o.move=d;add(o);}}
 if(a.acd<=1)for(int j=0;j<root.n;++j)if(j!=root.you&&!root.p[j].out&&(root.p[j].p-a.p).len()<5.2){Order o;o.attack=(root.p[j].p-a.p+(root.p[j].v-a.v)*6.5).unit();add(o);o.attack=(root.p[j].p-a.p).unit();add(o);}
 if(a.acd<=1)for(int k=0;k<root.nb;++k){const auto&b=root.b[k];if(b.fuse<10)continue;V rel=b.p+b.v*7.4-a.p-a.v*7.1;if(rel.len()<4.0&&(b.p-a.p).len()<6){Order o;o.attack=rel.unit();add(o);}}
 return out;}
 Order choose(){began=std::chrono::steady_clock::now();ctx.rootTick=root.tick;const auto&locked=root.p[root.you];if(locked.act==2||(locked.act==3&&locked.actt>=6))return Order{};auto all=candidates();std::vector<std::pair<double,int>> ranking;ranking.reserve(all.size());for(int k=0;k<int(all.size());++k){double v=score(all[k],3,5);ranking.push_back({v,k});if(expired()&&k>15)break;}std::sort(ranking.begin(),ranking.end(),[](auto a,auto b){return a.first>b.first;});Order best=all[ranking[0].second];double bestval=-1e20;
 int top=std::min(12,int(ranking.size()));for(int k=0;k<top&&!expired();++k){const Order&o=all[ranking[k].second];double v=score(o,3,7);if(v>bestval){bestval=v;best=o;}}
 if(root.alive()<=6&&!expired()){std::vector<Order> seeds{best};if(ranking.size()>1)seeds.push_back(all[ranking[1].second]);prepareResponses(seeds);if(!responses.empty()&&!expired()){double val=-1e20;Order refined=best;int count=root.alive()==2?int(ranking.size()):std::min(32,int(ranking.size()));for(int k=0;k<count&&!expired();++k){auto&o=all[ranking[k].second];double v=robustScore(o);if(v>val){val=v;refined=o;}}if(val>-1e19){best=refined;bestval=val;}}}
 // Jointly screen movement, charging, throw direction and fuse; then verify the promising combinations.
 const auto&a=root.p[root.you];if(a.bcd==0&&!a.act&&!expired()){
 std::vector<Order>bases;auto addbase=[&](Order o){if(o.attack.norm2()>0||(o.release.norm2()>0&&a.skill==2))return;for(auto&e:bases)if(e==o)return;bases.push_back(o);};addbase(best);
 for(int k=0;k<int(ranking.size())&&bases.size()<3;++k)addbase(all[ranking[k].second]);if(bases.empty()){Order o;o.move=movement(root,root.you,0,ctx);addbase(o);}
 Order escape=bases[0];escape.charge=0;escape.move=(-a.p-a.v*10).unit();addbase(escape);
 std::vector<V>dirs;for(int j=0;j<root.n;++j)if(j!=root.you&&!root.p[j].out&&(root.p[j].p-a.p).len()<15){V d=(root.p[j].p+root.p[j].v*12-a.p-a.v*12).unit();dirs.push_back(d);dirs.push_back(rotate(d,.25));dirs.push_back(rotate(d,-.25));if((root.p[j].p-a.p).len()<7){dirs.push_back((root.p[j].p-a.p).unit());dirs.push_back((root.p[j].p+root.p[j].v*3-a.p-a.v*3).unit());}}for(int k=0;k<12;++k)dirs.push_back(angle(k*PI/6));
 std::vector<std::pair<double,Order>> throws;
 for(const auto&base:bases)for(int fuse:{24,36,48})for(V d:dirs){if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count()>timeBudget-14)break;Order o=base;o.bomb=d;o.fuse=fuse;double val=responses.empty()?rollout(o,0,9):knownRollout(o,adversarySeat,responses[0],0,9);throws.push_back({val,o});}
 std::sort(throws.begin(),throws.end(),[](const auto&a,const auto&b){return a.first>b.first;});int tested=0;
 for(int k=0;k<int(throws.size())&&tested<14&&!expired();++k){Order o=throws[k].second;bool duplicate=false;for(int j=0;j<k;++j)if(throws[j].second.fuse==o.fuse&&(throws[j].second.bomb-o.bomb).norm2()<.025&&(throws[j].second.move-o.move).norm2()<.025&&throws[j].second.charge==o.charge)duplicate=true;if(duplicate)continue;double val=robustScore(o);++tested;if(val>bestval+.4){bestval=val;best=o;}}
 if(best.bomb.norm2()>0&&!expired()){Order base=best;for(int k=0;k<12&&!expired();++k){Order o=base;o.move=angle(k*PI/6);double val=robustScore(o);if(val>bestval+.2){bestval=val;best=o;}}}
 }
 return best;}
};
inline Context readContext(const World&s,const J&memory){Context c;if(memory.is_object()&&memory.contains("tick")&&integer(memory,"tick")==s.tick-6&&memory.contains("v")){auto&v=memory.at("v");for(int i=0;i<s.n&&i<int(v.size());++i){V old=jv(v.at(i));V change=s.p[i].v-old*std::pow(.94,6);double expected=(s.p[i].skill==1?.2:s.p[i].skill==2?.3:1)*.018*.94*(1-std::pow(.94,6))/.06;if(change.len()<expected*1.4&&change.len()>expected*.5&&s.p[i].act==0){c.inferred[i]=change.unit();c.confidence[i]=1;}}}return c;}
inline std::pair<J,J> decide(const J&obs,J&memory){Search search;search.root=World::read(obs);search.ctx=readContext(search.root,memory);Order order=search.choose();J v=J::array{};for(int i=0;i<search.root.n;++i)v.push_back(vj(search.root.p[i].v));J mem{{"tick",search.root.tick},{"v",v}};return {jsonOrder(order,search.root.p[search.root.you]),mem};}
}
