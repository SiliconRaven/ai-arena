#pragma once
#include "json.hpp"
#include "physics.hpp"
#include "python_random.hpp"
#include <array>
#include <charconv>
#include <set>
namespace knockback {
using mini_json::Json;
// Fixed-format conversion rounds the original binary64 value, not x*10**p.
// The latter changes half-way cases and is not equivalent to Python round().
inline double py_round(double value,int precision){
    if(!std::isfinite(value)||value==0)return value;
    if(precision>323)return value;
    if(precision < -308)return std::copysign(0.,value);
    if(precision<0){
        // Exact binary64 expansion lets negative digits round without a second rounding.
        std::array<char,1500>buf;
        auto encoded=std::to_chars(buf.data(),buf.data()+buf.size(),std::abs(value),std::chars_format::fixed,1074);
        if(encoded.ec!=std::errc())throw std::runtime_error("round conversion failed");
        std::string text(buf.data(),encoded.ptr);size_t dot=text.find('.');
        std::string whole=text.substr(0,dot),fraction=text.substr(dot+1);size_t cut=size_t(-precision);
        if(whole.size()<=cut)whole.insert(0,cut+1-whole.size(),'0');
        std::string kept=whole.substr(0,whole.size()-cut),tail=whole.substr(whole.size()-cut)+fraction;
        bool rest=tail.find_first_not_of('0',1)!=std::string::npos;
        bool up=tail[0]>'5'||(tail[0]=='5'&&(rest||(kept.back()-'0')%2));
        if(up){size_t i=kept.size();while(i&&kept[i-1]=='9'){kept[--i]='0';}if(i)++kept[i-1];else kept.insert(0,"1");}
        kept.append(cut,'0');double out;auto decoded=std::from_chars(kept.data(),kept.data()+kept.size(),out);
        if(decoded.ec!=std::errc())throw std::overflow_error("rounded value too large to represent");
        return std::copysign(out,value);
    }
    std::array<char,1500>buf;
    auto encoded=std::to_chars(buf.data(),buf.data()+buf.size(),value,std::chars_format::fixed,precision);
    if(encoded.ec!=std::errc())throw std::runtime_error("round conversion failed");
    double out;auto decoded=std::from_chars(buf.data(),encoded.ptr,out,std::chars_format::fixed);
    if(decoded.ec!=std::errc())throw std::runtime_error("round parse failed");
    return out;
}
inline double setup_num(const Json&j,const char*k){return j[k].as_double();}
inline int setup_int(const Json&j,const char*k){return int(j[k].as_int());}
inline Json setup_phases(const Json&r){auto&s=r["shrink"];Json::array phases;double half=setup_num(r["arena"],"half");int k=0;while(half>setup_num(s,"min_half")){int start=setup_int(s,"first_turn")+k*setup_int(s,"every");if(start>setup_int(r,"max_turns"))break;double target=std::max(setup_num(s,"min_half"),setup_num(r["arena"],"half")-(k+1)*setup_num(s,"step"));phases.push_back(Json{{"warn_turn",std::max(1,start-setup_int(s,"warn_turns"))},{"turn",start},{"end_turn",start+setup_int(s,"shrink_turns")-1},{"from",half},{"half",target}});half=target;++k;}return phases;}
inline Json setup_spawns(const std::string&seed,int n,const Json&r){auto rng=mini_json::PythonRandom::from_string("kb-lab:"+seed+":spawn");int offset=rng.randbelow(24);std::vector<int>slots;for(int i=0;i<n;++i)slots.push_back(i);rng.shuffle(slots);std::array<std::pair<double,double>,24>dirs;double c=(std::sqrt(6.)+std::sqrt(2.))/4,s=(std::sqrt(6.)-std::sqrt(2.))/4,x=1.,y=0.;for(auto&d:dirs){d=*unit(x,y);double nx=x*c-y*s;y=x*s+y*c;x=nx;}double radius=setup_num(r["arena"],"spawn_radius");int p=setup_int(r,"precision");Json::array out;for(int seat=0;seat<n;++seat){auto d=dirs[(offset+(2*slots[seat]*24+n)/(2*n))%24];out.push_back(Json::array{py_round(radius*d.first,p)+0.,py_round(radius*d.second,p)+0.});}return out;}
inline std::pair<double,double> setup_closest(double px,double py,const Json&w){double ax=setup_num(w,"x1"),ay=setup_num(w,"y1"),ex=setup_num(w,"x2")-ax,ey=setup_num(w,"y2")-ay;double length2=ex*ex+ey*ey;double t=length2==0?0.:std::clamp(((px-ax)*ex+(py-ay)*ey)/length2,0.,1.);return {ax+t*ex,ay+t*ey};}
inline double setup_gap(const Json&a,const Json&b){auto side=[](double ox,double oy,double px,double py,double qx,double qy){return (px-ox)*(qy-oy)-(py-oy)*(qx-ox);};auto cross=[&](const Json&w,const Json&o,const char*x,const char*y){return side(setup_num(w,"x1"),setup_num(w,"y1"),setup_num(w,"x2"),setup_num(w,"y2"),setup_num(o,x),setup_num(o,y));};if(cross(a,b,"x1","y1")*cross(a,b,"x2","y2")<0&&cross(b,a,"x1","y1")*cross(b,a,"x2","y2")<0)return 0.;double result=INFINITY;for(int i=0;i<4;++i){const Json&own=i<2?a:b;const Json&other=i<2?b:a;double x=setup_num(own,i%2?"x2":"x1"),y=setup_num(own,i%2?"y2":"y1");auto q=setup_closest(x,y,other);double dx=x-q.first,dy=y-q.second;result=std::min(result,std::sqrt(dx*dx+dy*dy));}return result;}
inline Json setup_quarter_turn(Json w){double x1=setup_num(w,"x1"),x2=setup_num(w,"x2");w["x1"]=-setup_num(w,"y1")+0.;w["y1"]=x1;w["x2"]=-setup_num(w,"y2")+0.;w["y2"]=x2;return w;}
inline Json setup_walls(const std::string&seed,const Json&r){auto&cfg=r["walls"];auto rng=mini_json::PythonRandom::from_string("kb-lab:"+seed+":walls");double half=setup_num(r["arena"],"half"),gap=setup_num(cfg,"clearance"),thick=setup_num(cfg,"thickness")/2,ring=setup_num(r["arena"],"spawn_radius"),body=setup_num(r["player"],"radius");int p=setup_int(r,"precision");double low=ring-body-gap-thick,high=ring+body+gap+thick;
 auto fits=[&](const Json&w,const std::vector<Json>&others){double x1=setup_num(w,"x1"),x2=setup_num(w,"x2"),y1=setup_num(w,"y1"),y2=setup_num(w,"y2");if(std::max(std::abs(x1),std::abs(y1))>half-gap-thick||std::max(std::abs(x2),std::abs(y2))>half-gap-thick)return false;auto q=setup_closest(0.,0.,w);double near=std::sqrt(q.first*q.first+q.second*q.second),far=std::max(std::sqrt(x1*x1+y1*y1),std::sqrt(x2*x2+y2*y2));if(near<gap+thick||!(far<low||near>high))return false;for(auto&o:others)if(setup_gap(w,o)<setup_num(cfg,"gap")+2*thick)return false;return true;};
 std::array<std::vector<Json>,2>quarters;for(int kind=0;kind<2;++kind)for(int count=0;count<setup_int(cfg,kind?"wall_per_quadrant":"spring_per_quadrant");++count){std::vector<Json>placed;for(auto&quarter:quarters)for(auto q:quarter)for(int i=0;i<4;++i){placed.push_back(q);q=setup_quarter_turn(q);}for(int attempt=0;attempt<setup_int(cfg,"tries");++attempt){auto uniform=[&](double a,double b){return a+(b-a)*rng.random();};double cx=uniform(0.,half),cy=uniform(0.,half),dx,dy;do{dx=uniform(-1.,1.);dy=uniform(-1.,1.);}while(dx*dx+dy*dy<.01||dx*dx+dy*dy>1.);auto u=*unit(dx,dy);double length=uniform(setup_num(cfg,"min_len"),setup_num(cfg,"max_len"))/2;Json wall{{"kind",kind?"wall":"spring"},{"x1",py_round(cx-u.first*length,p)+0.},{"y1",py_round(cy-u.second*length,p)+0.},{"x2",py_round(cx+u.first*length,p)+0.},{"y2",py_round(cy+u.second*length,p)+0.}};std::array<Json,4>copies;copies[0]=wall;for(int i=1;i<4;++i)copies[i]=setup_quarter_turn(copies[i-1]);bool okay=true;for(int i=0;i<4&&okay;++i){auto others=placed;for(int j=0;j<4;++j)if(i!=j)others.push_back(copies[j]);okay=fits(copies[i],others);}if(okay){quarters[kind].push_back(wall);break;}}}
 Json::array out;for(auto&quarter:quarters)for(int turn=0;turn<4;++turn)for(auto w:quarter){for(int i=0;i<turn;++i)w=setup_quarter_turn(w);w["id"]=int(out.size());out.push_back(w);}return out;}
inline Json setup_ranking(const std::string&seed,int turn,int D,const Json&out_tick,const Json&kills){int n=out_tick.size(),last=turn*D+1;std::vector<int>ends,draw,drawn(n);std::set<int,std::greater<int>>tiers;for(int i=0;i<n;++i){int end=out_tick[i].is_null()?last:int(out_tick[i].as_int());ends.push_back(end);tiers.insert(end);draw.push_back(i);}auto rng=mini_json::PythonRandom::from_string("kb-lab:"+seed+":rank");rng.shuffle(draw);for(int i=0;i<n;++i)drawn[draw[i]]=i;Json::array place(n),basis(n),detail(n);int better=0;for(int end:tiers){std::vector<int>tier;for(int i=0;i<n;++i)if(ends[i]==end)tier.push_back(i);std::stable_sort(tier.begin(),tier.end(),[&](int a,int b){int ka=kills[a].as_int(),kb=kills[b].as_int();return ka!=kb?ka>kb:drawn[a]<drawn[b];});for(int i=0;i<int(tier.size());++i){int seat=tier[i];place[seat]=better+i+1;std::string why;if(tier.size()==1)why=end==last?"survived":"out tick";else{bool unique=true;for(int q:tier)if(q!=seat&&kills[q]==kills[seat])unique=false;why=unique?"kills":"draw";}basis[seat]=why;detail[seat]=Json{{"tier_end",end},{"still_in",out_tick[seat].is_null()},{"kills",kills[seat]},{"draw",tier.size()>1?Json(drawn[seat]):Json(nullptr)},{"basis",why}};}better+=tier.size();}return Json{{"rank",place},{"rank_basis",basis},{"rank_detail",detail}};}
inline Json setup_header(const Json&submissions,const Json&seed,const Json&r){int n=submissions.size();if(n<2||n>setup_int(r,"max_players"))throw std::runtime_error("a game has 2 to "+std::to_string(setup_int(r,"max_players"))+" players, got "+std::to_string(n));if(setup_num(r["arena"],"spawn_radius")+setup_num(r["player"],"radius")>setup_num(r["arena"],"half"))throw std::runtime_error("the spawn circle must fit inside the square (arena.spawn_radius + player.radius <= half)");Json::array invalid;for(auto&s:submissions.as_array())invalid.push_back(s.is_string()?s:Json(nullptr));std::string text=seed.integer_decimal();return Json{{"game","knockback"},{"rules_version",r["version"]},{"seed",seed},{"players",n},{"tick_rate",r["tick_rate"]},{"decision_ticks",r["decision_ticks"]},{"max_turns",r["max_turns"]},{"arena",Json{{"half",setup_num(r["arena"],"half")}}},{"player_radius",r["player"]["radius"]},{"spawns",setup_spawns(text,n,r)},{"shrink",setup_phases(r)},{"walls",setup_walls(text,r)},{"invalid",invalid},{"rules",r}};}

}
