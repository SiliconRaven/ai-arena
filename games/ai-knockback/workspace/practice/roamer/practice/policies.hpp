#pragma once
// EP1 practice policies, included in the public local player package.
// Ported from programs/practice_opponents/*/bot.py; the Python originals remain
// the differential reference. Operation and stable tie ordering are intentional.
#include "../include/json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
namespace practice {
    using J=mini_json::Json;

    using V=std::array<double,2>;

    using Enemies=std::vector<const J*>;

    inline double n(const J&j,const char*k){return j.at(k).as_double();
    }
    inline double norm(double x,double y){return std::sqrt(x*x+y*y);
    }
    inline V unit(double x,double y){double d=norm(x,y);
        return d>1e-9?V{x/d,y/d}:V{0.,0.};
    }
    inline double edge(double x,double y){return std::max(std::abs(x),std::abs(y));
    }
    inline V outward(double x,double y){return std::abs(x)>=std::abs(y)?V{x>=0?1.:-1.,0.}:V{0.,y>=0?1.:-1.};
    }
    inline J vec(V v){return J::array{v[0],v[1]};
    }
    inline V xy(const J&p){return {n(p,"x"),n(p,"y")};
    }
    inline double distance(const J&a,const J&b){return norm(n(a,"x")-n(b,"x"),n(a,"y")-n(b,"y"));
    }
    inline bool truth(const J&v){return !v.is_null()&&(!v.is_array()||!v.empty())&&(!v.is_object()||!v.empty())&&(!v.is_string()||!v.empty())&&(!v.is_bool()||v.as_bool())&&(!v.is_number()||v.as_double()!=0);
    }
    inline bool has(const J&o,const char*k){return o.contains(k)&&truth(o.at(k));
    }
    inline J merged(J a,const J&b){if(b.is_object())for(auto&kv:b.as_object())a[kv.first]=kv.second;
        return a;
    }
    struct WallHit {double distance,nx,ny;
        std::string kind;
    };

    class Policy {
        const J&o;
        const J&r;
        const J&m;
        Enemies enemies;
        std::string name;

        double safe_half(double lead=4)const{double half=n(o.at("arena"),"half");
            auto&up=o.at("arena").at("next");
            if(truth(up)&&n(up,"turn")-n(o,"turn")<=lead)half=std::min(half,n(up,"half"));
            return half;
        }
        double slide()const{double f=n(r.at("physics"),"friction");
            return (1-f)/f;
        }
        std::optional<WallHit> wall_hit(double x,double y,double ux,double uy)const{
            double reach=n(r.at("player"),"radius")+n(r.at("walls"),"thickness")/2;
            std::optional<WallHit>best;

            for(auto&w:o.at("walls").as_array()){
                double ax=n(w,"x1"),ay=n(w,"y1"),bx=n(w,"x2"),by=n(w,"y2");
                auto e=unit(bx-ax,by-ay);
                double ex=e[0],ey=e[1],length=norm(bx-ax,by-ay),nx=-ey,ny=ex,h0=(x-ax)*nx+(y-ay)*ny,un=ux*nx+uy*ny;

                std::vector<WallHit>hits;

                if(std::abs(h0)>=reach-.05&&un*h0<0){double s=std::max(0.,std::abs(h0)-reach)/std::abs(un),t=(x+ux*s-ax)*ex+(y+uy*s-ay)*ey;
                    if(0<=t&&t<=length){double side=h0>0?1.:-1.;
                        hits.push_back({s,nx*side,ny*side,w.at("kind").as_string()});
                    }}
                for(V c:{V{ax,ay},V{bx,by}}){double fx=x-c[0],fy=y-c[1],b=fx*ux+fy*uy,cc=fx*fx+fy*fy-reach*reach;
                    if(cc>-.1&&b<0&&b*b-cc>=0){double s=std::max(0.,-b-std::sqrt(b*b-cc));
                        auto u=unit(fx+ux*s,fy+uy*s);
                        hits.push_back({s,u[0],u[1],w.at("kind").as_string()});
                    }}
                for(auto&h:hits)if(!best||h.distance<best->distance)best=h;

            }return best;

        }
        V stop(const J&p,V extra={0.,0.})const{
            double k=slide(),vx=n(p,"vx")+extra[0],vy=n(p,"vy")+extra[1],speed=norm(vx,vy);

            if(speed<1e-6||o.at("walls").empty())return {n(p,"x")+vx*k,n(p,"y")+vy*k};

            double ux=vx/speed,uy=vy/speed;
            auto hit=wall_hit(n(p,"x"),n(p,"y"),ux,uy);

            if(!hit||hit->distance>=speed*k)return {n(p,"x")+vx*k,n(p,"y")+vy*k};

            double s=hit->distance,nx=hit->nx,ny=hit->ny,left=speed*(1-s/(speed*k)),wx=ux*left,wy=uy*left,inward=wx*nx+wy*ny;

            double out=hit->kind=="wall"?n(r.at("walls"),"restitution")*-inward:std::min(n(r.at("spring"),"factor")*-inward,n(r.at("spring"),"max_speed"));
            wx=wx+(out-inward)*nx;
            wy=wy+(out-inward)*ny;
            return {n(p,"x")+ux*s+wx*k,n(p,"y")+uy*s+wy*k};

        }
        bool can_dash(double least)const{return m.at("skill")==J("dash")&&n(m,"charge")>=least;
        }
        bool safe_dash(V u,double least)const{if(!can_dash(least))return false;
            double speed=n(r.at("dash"),"speed")*n(m,"charge");
            auto p=stop(m,{u[0]*speed,u[1]*speed});
            return edge(p[0],p[1])<safe_half()-.5;
        }
        bool lapsing()const{return n(m,"charge")>0&&n(m,"held")+n(r,"decision_ticks")>=n(r.at("bar"),"overcharge_ticks");
        }
        J best_dash(V u,double now)const{
            double h=std::sqrt(.5),speed=n(r.at("dash"),"speed")*n(m,"charge"),best=0;
            V direction{};
            bool found=false;

            for(V cs:{V{1.,0.},V{h,h},V{h,-h},V{0.,1.},V{0.,-1.}}){V d{u[0]*cs[0]-u[1]*cs[1],u[0]*cs[1]+u[1]*cs[0]};
                auto p=stop(m,{d[0]*speed,d[1]*speed});
                double e=edge(p[0],p[1]);
                if(!found||e<best-.5){best=e;
                    direction=d;
                    found=true;
                }}
            if(best>=now||best>n(o.at("arena"),"half")-.5)return nullptr;
            return vec(direction);

        }
        J recover(double margin)const{auto s=stop(m);
            double safe=safe_half();
            if(edge(s[0],s[1])<=safe-margin)return nullptr;
            double hx=std::abs(s[0])>safe-margin?-std::copysign(1.,s[0]):0.,hy=std::abs(s[1])>safe-margin?-std::copysign(1.,s[1]):0.;
            auto home=unit(hx,hy);
            J order{{"move",vec(home)}};
            if(edge(s[0],s[1])>safe-.5){if(can_dash(.1)){J d=best_dash(home,edge(s[0],s[1]));
                    if(truth(d))order["dash"]=d;
                }else order["charge"]="dash";
            }return order;
        }
        J regrowth_escape(double margin=.8)const{
            double reach=n(r.at("player"),"radius")+n(r.at("walls"),"thickness")/2+margin,soon=n(r.at("walls"),"warn_ticks")+n(r,"decision_ticks");

            for(auto&b:o.at("broken").as_array()){if(n(b,"at")-n(o,"tick")>soon)continue;
                double ax=n(b,"x1"),ay=n(b,"y1"),ex=n(b,"x2")-ax,ey=n(b,"y2")-ay,t=std::max(0.,std::min(1.,((n(m,"x")-ax)*ex+(n(m,"y")-ay)*ey)/(ex*ex+ey*ey))),nx=n(m,"x")-(ax+t*ex),ny=n(m,"y")-(ay+t*ey);
                if(norm(nx,ny)<reach){V out=norm(nx,ny)>1e-6?unit(nx,ny):unit(-ey,ex),c=unit(-n(m,"x"),-n(m,"y"));
                    return J{{"move",vec(unit(out[0]+.3*c[0],out[1]+.3*c[1]))}};
                }}return nullptr;

        }
        double reach(double charge,int ticks=6)const{double f=n(r.at("physics"),"friction"),s=n(r.at("dash"),"speed")*charge;
            return s*(1-f)*(1-std::pow(1-f,ticks))/f;
        }
        std::pair<V,double> aim(const J&t,double speed)const{double dx=n(t,"x")-n(m,"x"),dy=n(t,"y")-n(m,"y"),ticks=std::max(0.,norm(dx,dy)-2)/std::max(speed,.05);
            return {unit(dx+(n(t,"vx")-n(m,"vx"))*ticks,dy+(n(t,"vy")-n(m,"vy"))*ticks),ticks};
        }
        bool thrown(const J&t,V u,double gain,double margin=.5)const{auto s=stop(t,{u[0]*gain,u[1]*gain});
            return edge(s[0],s[1])>n(o.at("arena"),"half")+margin;
        }
        bool lethal(const J&t,V u,double speed,double margin=.5)const{return thrown(t,u,speed*(1+n(r.at("physics"),"restitution"))/2,margin);
        }
        bool miss_safe(V u,double speed)const{auto s=stop(m,{u[0]*speed,u[1]*speed});
            return edge(s[0],s[1])<safe_half()-.5;
        }
        Enemies sorted(const Enemies&es,const J&from)const{Enemies out=es;
            std::stable_sort(out.begin(),out.end(),[&](auto*a,auto*b){return distance(*a,from)<distance(*b,from);});
            return out;
        }
        const J* incoming(int horizon=10)const{for(auto*ep:enemies){auto&e=*ep;
                double vx=n(e,"vx")-n(m,"vx"),vy=n(e,"vy")-n(m,"vy"),v2=vx*vx+vy*vy;
                if(v2<.35*.35)continue;
                double rx=n(e,"x")-n(m,"x"),ry=n(e,"y")-n(m,"y"),t=-(rx*vx+ry*vy)/v2;
                if(0<t&&t<=horizon&&norm(rx+vx*t,ry+vy*t)<2.2)return ep;
            }return nullptr;
        }
        V sidestep(const J&t)const{auto p=unit(-n(t,"vy"),n(t,"vx"));
            if(p[0]*n(m,"x")+p[1]*n(m,"y")>0){p[0]=-p[0];
                p[1]=-p[1];
            }return p;
        }
        J strike(const Enemies&es,double min_charge,bool need_lethal,bool push_out=false,int ticks=4)const{
            if(!can_dash(min_charge))return nullptr;
            double speed=n(r.at("dash"),"speed")*n(m,"charge");
            for(auto*tp:sorted(es,m)){auto&t=*tp;
                V u=aim(t,speed).first;
                if(distance(t,m)-2>reach(n(m,"charge"),ticks))continue;
                if(need_lethal&&!lethal(t,u,speed))continue;
                V out=outward(n(t,"x"),n(t,"y"));
                if(push_out&&u[0]*out[0]+u[1]*out[1]<=0)continue;
                if(miss_safe(u,speed)||distance(t,m)<3)return J{{"move",vec(u)},{"dash",vec(u)}};
            }return nullptr;

        }
        const J* danger()const{const J*best=nullptr;
            for(auto*e:enemies){if(n(*e,"charge")<.3)continue;
                double d=distance(*e,m);
                bool threat=e->at("skill")==J("hammer")?d<=n(r.at("hammer"),"reach")+1.5:d-2<=reach(n(*e,"charge"))+.5;
                if(threat&&(!best||d<distance(*best,m)))best=e;
            }return best;
        }
        V away(const J&e)const{auto a=unit(n(m,"x")-n(e,"x"),n(m,"y")-n(e,"y")),c=unit(-n(m,"x"),-n(m,"y"));
            return unit(a[0]+.7*c[0],a[1]+.7*c[1]);
        }
        bool in_swing(const J&h,double margin=.8)const{auto&cfg=r.at("attack");
            double t=n(h,"act_ticks"),dx=n(m,"x")+n(m,"vx")*t-n(h,"x")-n(h,"vx")*t,dy=n(m,"y")+n(m,"vy")*t-n(h,"y")-n(h,"vy")*t;
            if(norm(dx,dy)>n(cfg,"reach")+margin)return false;
            auto u=unit(dx,dy);
            return u[0]*h.at("aim").at(0).as_double()+u[1]*h.at("aim").at(1).as_double()>=n(cfg,"cone_cos")-.2;
        }
        J dodge_jab()const{for(auto*hp:enemies){auto&h=*hp;
                if(h.at("act")==J("jab")&&in_swing(h)){double px=-h.at("aim").at(1).as_double(),py=h.at("aim").at(0).as_double();
                    if(px*n(m,"x")+py*n(m,"y")>0){px=-px;
                        py=-py;
                    }auto a=unit(n(m,"x")-n(h,"x"),n(m,"y")-n(h,"y")),out=unit(px+.5*a[0],py+.5*a[1]);
                    J order{{"move",vec(out)}};
                    if(safe_dash(out,.15))order["dash"]=vec(out);
                    return order;
                }}return nullptr;
        }
        J smash(double min_charge,bool need_lethal=false)const{auto&h=r.at("hammer");
            if(m.at("skill")!=J("hammer")||n(m,"charge")<min_charge)return nullptr;
            for(auto*tp:sorted(enemies,m)){auto&t=*tp;
                double dx=n(t,"x")-n(m,"x"),dy=n(t,"y")-n(m,"y");
                if(norm(dx,dy)>n(h,"reach")-.4)continue;
                auto u=unit(dx,dy);
                if(need_lethal&&!thrown(t,u,n(h,"power")*n(m,"charge")))continue;
                return J{{"hammer",vec(u)}};
            }return nullptr;
        }
        J around_walls(J order)const{
            if(!has(order,"move"))return order;
            auto move=order.at("move");
            auto u=unit(move.at(0).as_double(),move.at(1).as_double());
            double mx=u[0],my=u[1],reach=n(r.at("player"),"radius")+n(r.at("walls"),"thickness")/2+.3;

            for(auto&w:o.at("walls").as_array()){double ax=n(w,"x1"),ay=n(w,"y1"),ex=n(w,"x2")-ax,ey=n(w,"y2")-ay,t=std::max(0.,std::min(1.,((n(m,"x")-ax)*ex+(n(m,"y")-ay)*ey)/(ex*ex+ey*ey))),nx=n(m,"x")-(ax+t*ex),ny=n(m,"y")-(ay+t*ey);
                if(norm(nx,ny)>=reach||norm(nx,ny)<1e-9)continue;
                auto v=unit(nx,ny);
                nx=v[0];
                ny=v[1];
                double into=mx*nx+my*ny;
                if(into<0){mx=mx-into*nx;
                    my=my-into*ny;
                    if(norm(mx,my)<.2){mx=-ny;
                        my=nx;
                    }v=unit(mx+.2*nx,my+.2*ny);
                    mx=v[0];
                    my=v[1];
                }}
            order["move"]=vec({mx,my});
            return order;

        }
        bool jab_ok(const J&t,V u)const{
            if(name=="center"||name=="bomber")return true;

            if(name=="anvil")return n(m,"hammer_cd")>0||m.at("skill")!=J("hammer")||n(m,"charge")<.4;

            auto out=outward(n(t,"x"),n(t,"y"));

            if(name=="dasher")return u[0]*out[0]+u[1]*out[1]>0&&!can_dash(1.);

            if(thrown(t,u,n(r.at("attack"),"power")))return true;

            return edge(n(t,"x"),n(t,"y"))>.6*safe_half()&&u[0]*out[0]+u[1]*out[1]>.5;

        }
        J jab(double slack=1.2)const{auto&a=r.at("attack");
            if(n(m,"attack_cd")>0||!m.at("act").is_null())return nullptr;
            double lead=n(a,"windup_ticks");
            for(auto*tp:sorted(enemies,m)){auto&t=*tp;
                double dx=n(t,"x")+n(t,"vx")*lead-n(m,"x")-n(m,"vx")*lead,dy=n(t,"y")+n(t,"vy")*lead-n(m,"y")-n(m,"vy")*lead;
                if(norm(dx,dy)>n(a,"reach")-.3+slack)continue;
                auto u=unit(dx,dy);
                if(jab_ok(t,u))return J{{"attack",vec(u)}};
            }return nullptr;
        }
        J spend()const{if(!lapsing()||!m.at("act").is_null()||enemies.empty())return nullptr;
            auto&near=*sorted(enemies,m).front();
            auto at=unit(n(near,"x")-n(m,"x"),n(near,"y")-n(m,"y"));
            if(m.at("skill")==J("hammer"))return J{{"hammer",vec(at)}};
            for(V u:{at,unit(-n(m,"x"),-n(m,"y"))})if(norm(u[0],u[1])>0&&safe_dash(u,0.))return J{{"dash",vec(u)}};
            return nullptr;
        }
        V bomb_stop(const J&b)const{double f=n(r.at("bomb"),"friction"),k=(1-f)/f;
            return {n(b,"x")+n(b,"vx")*k,n(b,"y")+n(b,"vy")*k};
        }
        J bomb_escape(int soon=14,double margin=1.)const{double blast=n(r.at("bomb"),"blast_radius"),best=0;
            V pos{};
            bool found=false;
            for(auto&b:o.at("bombs").as_array()){if(n(b,"fuse")>soon)continue;
                auto p=n(b,"fuse")>4?bomb_stop(b):xy(b);
                double d=norm(n(m,"x")-p[0],n(m,"y")-p[1]);
                if(d<blast+margin&&(!found||d<best)){best=d;
                    pos=p;
                    found=true;
                }}if(!found)return nullptr;
            auto a=best>1e-6?unit(n(m,"x")-pos[0],n(m,"y")-pos[1]):unit(-n(m,"x"),-n(m,"y")),c=unit(-n(m,"x"),-n(m,"y")),out=unit(a[0]+.6*c[0],a[1]+.6*c[1]);
            J order{{"move",vec(out)}};
            if(best<blast*.7&&safe_dash(out,.2))order["dash"]=vec(out);
            return order;
        }
        J bat_bomb()const{auto&h=r.at("hammer");
            if(m.at("skill")!=J("hammer")||n(m,"charge")<.3)return nullptr;
            for(auto&b:o.at("bombs").as_array()){double bx=n(b,"x")-n(m,"x"),by=n(b,"y")-n(m,"y");
                if(norm(bx,by)>n(h,"reach")-.3||!(0<n(b,"fuse")&&n(b,"fuse")<=30))continue;
                auto aim_at=unit(bx,by);
                for(auto*e:sorted(enemies,b)){auto u=unit(n(*e,"x")-n(m,"x"),n(*e,"y")-n(m,"y"));
                    if(u[0]*aim_at[0]+u[1]*aim_at[1]>=n(h,"cone_cos")+.25){aim_at=u;
                        break;
                    }}return J{{"hammer",vec(aim_at)}};
            }return nullptr;
        }
        double bomb_slide(double speed,int ticks)const{double f=n(r.at("bomb"),"friction");
            return speed*(1-f)*(1-std::pow(1-f,ticks))/f;
        }
        J lob(const J&t,double short_by=1.7)const{
            auto&c=r.at("bomb");
            double gap=n(r.at("player"),"radius")+n(c,"radius")+n(c,"spawn_gap"),tx=n(t,"x")+n(t,"vx")*12,ty=n(t,"y")+n(t,"vy")*12,dx=tx-n(m,"x"),dy=ty-n(m,"y"),d=norm(dx,dy);
            auto u=unit(dx,dy);
            double want=d-gap-short_by,speed=n(c,"throw_speed");
            if(want<=0||want>bomb_slide(speed,400)*.97)return nullptr;
            int ticks=c.at("fuse_min").get<int>(),max=c.at("fuse_max").get<int>();
            for(;ticks<=max;++ticks)if(bomb_slide(speed,ticks)>=want)break;
            if(ticks>max)return nullptr;
            auto wall=wall_hit(n(m,"x"),n(m,"y"),u[0],u[1]);
            if(wall&&wall->distance<gap+want+n(c,"blast_radius"))return nullptr;
            V land{n(m,"x")+u[0]*(gap+want),n(m,"y")+u[1]*(gap+want)};
            double k=slide();
            V mine{n(m,"x")+n(m,"vx")*k,n(m,"y")+n(m,"vy")*k};
            if(norm(land[0]-mine[0],land[1]-mine[1])<n(c,"blast_radius")+2.)return nullptr;
            return J{{"bomb",J{{"dir",vec(u)},{"fuse",ticks}}}};

        }
        J breach(const J&e)const{auto&c=r.at("bomb");
            double dx=n(e,"x")-n(m,"x"),dy=n(e,"y")-n(m,"y");
            auto u=unit(dx,dy);
            auto hit=wall_hit(n(m,"x"),n(m,"y"),u[0],u[1]);
            if(!hit||hit->kind!="wall"||hit->distance>norm(dx,dy)||hit->distance<n(c,"blast_radius")+1.5)return nullptr;
            double gap=n(r.at("player"),"radius")+n(c,"radius")+n(c,"spawn_gap"),want=std::max(0.,hit->distance-gap);
            for(int ticks=c.at("fuse_min").get<int>();ticks<=c.at("fuse_max").get<int>();++ticks)if(bomb_slide(n(c,"throw_speed"),ticks)>=want)return J{{"bomb",J{{"dir",vec(u)},{"fuse",ticks}}}};
            return nullptr;
        }
        V steer(double gx,double gy)const{auto s=stop(m);
            return {gx-s[0],gy-s[1]};
        }
        // Three modest styles share obstacle and edge recovery but make different tactical choices.
        J moderate_plan()const{
            if(m.at("out").as_bool()||enemies.empty())return J::object{};
            J order=recover(name=="striker"?2.:2.5);
            if(truth(order)){
                if(name=="roamer"){order.erase("charge");order.erase("dash");}
                return order;
            }
            order=regrowth_escape();
            if(truth(order))return order;
            order=bomb_escape();
            if(truth(order)){
                if(name=="roamer")order.erase("dash");
                return order;
            }

            const J&target=*sorted(enemies,m).front();
            double d=distance(target,m);
            V toward=unit(n(target,"x")-n(m,"x"),n(target,"y")-n(m,"y"));
            V home=unit(-n(m,"x"),-n(m,"y"));
            bool can_attack=n(m,"attack_cd")==0&&m.at("act").is_null();
            if(name=="striker"){
                if(d<7&&safe_dash(toward,.48))return J{{"dash",vec(toward)},{"move",vec(toward)}};
                if(can_attack&&d<n(r.at("attack"),"reach")+.4)
                    return J{{"attack",vec(toward)},{"move",vec(toward)}};
                if(lapsing()&&safe_dash(home,.2))return J{{"dash",vec(home)}};
                V move=unit(toward[0]+.3*home[0],toward[1]+.3*home[1]);
                bool charge=d<8||m.at("skill")==J("dash");
                return J{{"move",vec(move)},{"charge",charge?J("dash"):J()}};
            }
            if(name=="warden"){
                order=smash(.4);
                if(truth(order))return order;
                if(lapsing()&&m.at("skill")==J("hammer"))return J{{"hammer",vec(toward)}};
                if(can_attack&&d<n(r.at("attack"),"reach")+.3&&
                   (m.at("skill")!=J("hammer")||n(m,"charge")<.25))
                    return J{{"attack",vec(toward)}};
                V p=stop(m);
                V goal{.3*n(target,"x"),.3*n(target,"y")};
                V move=norm(goal[0]-p[0],goal[1]-p[1])>1.?unit(goal[0]-p[0],goal[1]-p[1]):V{0.,0.};
                bool charge=n(m,"hammer_cd")==0&&(d<7||m.at("skill")==J("hammer"));
                return J{{"move",vec(move)},{"charge",charge?J("hammer"):J()}};
            }
            if(can_attack&&d<n(r.at("attack"),"reach")+.3)return J{{"attack",vec(toward)}};
            V away=unit(-toward[0]+.8*home[0],-toward[1]+.8*home[1]);
            double direction=o.at("you").get<int>()%2==0?1.:-1.;
            V side{-direction*toward[1],direction*toward[0]};
            V move=d<5?away:d>10?toward:unit(side[0]+home[0],side[1]+home[1]);
            order=J{{"move",vec(move)}};
            if(n(m,"bomb_cd")==0&&d>=5&&d<=12){
                J bomb=lob(target);
                if(truth(bomb))order=merged(order,bomb);
            }
            return order;
        }
        J plan()const{
            if(m.at("out").as_bool()||enemies.empty())return J::object{};

            J order=recover(name=="dasher"?1.:name=="anvil"?1.5:name=="center"?2.5:2.);
            if(truth(order))return order;

            order=regrowth_escape();
            if(truth(order))return order;

            if(name=="anvil"){order=bat_bomb();
                if(truth(order))return order;
                order=smash(.6);
                if(truth(order))return order;
            }
            if(name!="dasher"){order=dodge_jab();
                if(truth(order))return order;
            }
            order=name=="dasher"?bomb_escape(8,0.):bomb_escape();
            if(truth(order))return order;

            const J&near=*sorted(enemies,m).front();

            if(name=="dasher"){
                order=strike(enemies,1.,false,true,6);
                if(truth(order))return order;
                auto u=aim(near,n(r.at("dash"),"speed")*std::max(n(m,"charge"),.3)).first;

                if(lapsing()&&can_dash(.3)&&miss_safe(u,n(r.at("dash"),"speed")*n(m,"charge")))return J{{"move",vec(u)},{"dash",vec(u)}};

                return J{{"move",vec(u)},{"charge",distance(near,m)<7?J("dash"):J()}};

            }
            if(name=="anvil"){
                auto rush=incoming();
                if(rush&&m.at("skill")==J("hammer")&&n(m,"charge")>=.4)return J{{"hammer",vec(unit(n(*rush,"x")-n(m,"x"),n(*rush,"y")-n(m,"y")))}};

                auto g=stop(m);
                if(norm(n(near,"x"),n(near,"y"))<.35*safe_half()&&distance(near,m)<6){g[0]=g[0]-n(near,"x");
                    g[1]=g[1]-n(near,"y");
                }
                return J{{"move",norm(g[0],g[1])>.4?vec({-g[0],-g[1]}):J()},{"charge",distance(near,m)<6?J("hammer"):J()}};

            }
            if(name=="bomber"){
                if(enemies.size()==1){order=strike(enemies,.5,true);
                    if(truth(order))return order;
                }
                double safe=safe_half();
                J move;
                if(distance(near,m)<4)move=vec(away(near));
                else{auto g=stop(m);
                    move=norm(g[0],g[1])>.3*safe?vec({-g[0],-g[1]}):J();
                }
                order=J{{"move",move},{"charge",enemies.size()==1?J("dash"):J()}};

                if(n(m,"bomb_cd")==0){double blast=n(r.at("bomb"),"blast_radius");
                    auto worth=[&](const J*t){int crowd=0;
                        for(auto*e:enemies)if(e!=t&&distance(*e,*t)<blast)++crowd;
                        return crowd+edge(n(*t,"x"),n(*t,"y"))/safe;
                    };
                    auto targets=enemies;
                    std::stable_sort(targets.begin(),targets.end(),[&](auto*a,auto*b){return worth(a)>worth(b);});
                    for(auto*t:targets){auto bomb=lob(*t);
                        if(truth(bomb))return merged(order,bomb);
                    }auto bomb=breach(near);
                    if(truth(bomb))return merged(order,bomb);
                }
                return order;

            }
            auto threat=incoming();
            if(threat){auto side=sidestep(*threat);
                order=J{{"move",vec(side)}};
                if(safe_dash(side,.2))order["dash"]=vec(side);
                else if(name=="opportunist")order["charge"]="dash";
                return order;
            }
            if(name=="center"){
                order=strike(Enemies{&near},.3,!(distance(near,m)<3.5));
                if(truth(order))return order;
                order=strike(enemies,.5,true);
                if(truth(order))return order;

                auto foe=danger();
                auto g=stop(m);
                order=J{{"move",foe&&n(m,"charge")<n(*foe,"charge")?vec(away(*foe)):(norm(g[0],g[1])>.5?vec({-g[0],-g[1]}):J())}};
                if(distance(near,m)<5.5)order["charge"]="dash";
                return order;

            }
            order=strike(enemies,.4,true);
            if(truth(order))return order;
            order=smash(.5,true);
            if(truth(order))return order;
            order=bat_bomb();
            if(truth(order))return order;

            J held=truth(m.at("skill"))?m.at("skill"):J("dash");
            bool busy=distance(near,m)<7;
            auto foe=danger();
            if(foe&&n(*foe,"charge")>n(m,"charge"))return J{{"move",vec(away(*foe))},{"charge",busy?held:J()}};

            double safe=safe_half();
            auto worth=[&](const J*p){return edge(n(*p,"x"),n(*p,"y"))+.3*std::min(std::abs(n(*p,"x")),std::abs(n(*p,"y")))-.15*distance(*p,m);
            };
            const J*target=enemies.front();
            for(auto*p:enemies)if(worth(p)>worth(target))target=p;

            if(edge(n(*target,"x"),n(*target,"y"))>.5*safe){if(n(m,"charge")<.3)held=norm(n(*target,"vx"),n(*target,"vy"))<.08&&truth(target->at("skill"))?"hammer":"dash";

                auto out=std::min(std::abs(n(*target,"x")),std::abs(n(*target,"y")))>.6*safe?unit(std::copysign(1.,n(*target,"x")),std::copysign(1.,n(*target,"y"))):outward(n(*target,"x"),n(*target,"y"));
                double back=held==J("hammer")?2.2:3.5;
                V spot{n(*target,"x")-out[0]*back,n(*target,"y")-out[1]*back};
                bool nearly=norm(spot[0]-n(m,"x"),spot[1]-n(m,"y"))<6;
                return J{{"move",vec(steer(spot[0],spot[1]))},{"charge",nearly?held:J()}};

            }
            // CPython 3.12 (the pinned bot Docker runtime) uses compensated float sum.
            auto sum_axis=[&](const char*axis){double hi=0.,lo=0.;
                for(auto*p:enemies){double x=n(*p,axis),t=hi+x;
                    lo+=std::abs(hi)>=std::abs(x)?(hi-t)+x:(x-t)+hi;
                    hi=t;
                }return hi+lo;
            };

            double cx=sum_axis("x")/enemies.size(),cy=sum_axis("y")/enemies.size();
            auto a=unit(-cx,-cy);
            return J{{"move",vec(steer(a[0]*.3*safe,a[1]*.3*safe))},{"charge",busy?held:J()}};

        }
        public:
        Policy(const J&obs,std::string style):o(obs),r(obs.at("rules")),m(obs.at("me")),name(std::move(style)){for(size_t i=0;i<o.at("players").size();++i)if(i!=size_t(o.at("you").get<int>())&&!o.at("players").at(i).at("out").as_bool())enemies.push_back(&o.at("players").at(i));
        }
        J step()const{if(name=="striker"||name=="warden"||name=="roamer")return around_walls(moderate_plan());
            J order=plan();
            if(!truth(order))order=J::object{};
            if(!m.at("out").as_bool()&&!has(order,"dash")&&!has(order,"hammer")&&!has(order,"bomb")&&!has(order,"attack"))order=merged(order,spend());
            if(!m.at("out").as_bool()&&!has(order,"dash")&&!has(order,"hammer")&&!has(order,"bomb")&&edge(n(m,"x"),n(m,"y"))<safe_half()-2.5)order=merged(order,jab());
            return around_walls(order);
        }
    };

} // namespace practice
