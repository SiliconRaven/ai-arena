// The public baseline policy: walk to the nearest rival, jab when in reach, charge and dash close in, step back
// from the edge, walls and nearby bombs. `step(observation)` returns the order; it keeps no memory.
#pragma once
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
using mini_json::Json;
using J=Json;
double number(const J& j,const char* k) { return j[k].as_double(); }
J vec(double x,double y) {return J::array{x,y};}
J step(const J& obs) {
    const auto& me=obs["me"]; const J* target=nullptr;
    if(me["out"].as_bool()) return J::object{};
    double best=std::numeric_limits<double>::infinity();
    for(size_t i=0;i<obs["players"].size();++i) {
        const auto& p=obs["players"][i];
        if(i==size_t(obs["you"].as_int()) || p["out"].as_bool()) continue;
        double dx=number(p,"x")-number(me,"x"),dy=number(p,"y")-number(me,"y"),d=dx*dx+dy*dy;
        if(d<best) {best=d; target=&p;}
    }
    if(!target) return J::object{};
    double dx=number(*target,"x")-number(me,"x"),dy=number(*target,"y")-number(me,"y"),mx=dx,my=dy;
    for(const auto& bomb:obs["bombs"].as_array()) {
        double bx=number(me,"x")-number(bomb,"x"),by=number(me,"y")-number(bomb,"y");
        if(bx*bx+by*by<16) return J{{"move",vec(bx,by)}};
    }
    J::array soon;
    for(const auto& b:obs["broken"].as_array())
        if(number(b,"at")-number(obs,"tick")<=number(obs["rules"]["walls"],"warn_ticks")) soon.push_back(b);
    auto walls=obs["walls"].as_array(); walls.insert(walls.end(),soon.begin(),soon.end());
    for(const auto& wall:walls) {
        double ax=number(wall,"x1"),ay=number(wall,"y1"),ex=number(wall,"x2")-ax,ey=number(wall,"y2")-ay;
        double t=std::max(0.0,std::min(1.0,((number(me,"x")-ax)*ex+(number(me,"y")-ay)*ey)/(ex*ex+ey*ey)));
        double wx=number(me,"x")-(ax+t*ex),wy=number(me,"y")-(ay+t*ey);
        if(wx*wx+wy*wy>=2.5*2.5) continue;
        if(std::find(soon.begin(),soon.end(),wall)!=soon.end()) return J{{"move",vec(wx,wy)}};
        double into=mx*wx+my*wy;
        if(into<0) {double w2=wx*wx+wy*wy; mx=mx-into*wx/w2+0.3*wx; my=my-into*wy/w2+0.3*wy;}
    }
    if(std::max(std::abs(number(me,"x")),std::abs(number(me,"y")))>number(obs["arena"],"half")-2)
        return J{{"move",vec(-number(me,"x"),-number(me,"y"))}};
    bool dash=me["skill"].is_string() && me["skill"].as_string()=="dash";
    if(number(me,"attack_cd")==0 && me["act"].is_null() && std::sqrt(dx*dx+dy*dy)<number(obs["rules"]["attack"],"reach"))
        return J{{"attack",vec(dx,dy)},{"charge",dash?J("dash"):J(nullptr)}};
    if(number(me,"charge")>=1) {
        if(std::sqrt(dx*dx+dy*dy)<10) return J{{"dash",vec(dx,dy)}};
        return J{{"move",vec(mx,my)}};
    }
    bool charge=dash || (number(me,"bar")>=1 && std::sqrt(dx*dx+dy*dy)<12);
    return J{{"move",vec(mx,my)},{"charge",charge?J("dash"):J(nullptr)}};
}
