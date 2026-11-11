#pragma once
// Deterministic reference engine. Keep expression order aligned with game/engine.py.
#include "json.hpp"
#include "physics.hpp"
#include "setup.hpp"
#include <cstdio>
#include "sha2.hpp"
#include <set>
namespace knockback {
    using J=mini_json::Json;

    inline J arr(){return J::array{};
    }
    inline J vec(double x,double y){return J::array{x,y};
    }
    inline double num(const J&j,const char*k){return j.at(k).as_double();
    }
    inline int integer(const J&j,const char*k){return j.at(k).get<int>();
    }
    inline bool has(const J&j,const std::string&k){return j.contains(k)&&!j.at(k).is_null();
    }
    // Decimal conversion rounds the exact binary64 value, matching Python round(x,n).
    // Multiplying by 10**n before nearbyint is not equivalent at decimal halfway cases.
    inline double pyround(double x,int n){return py_round(x,n);
    }

    inline std::pair<double,double> nearest(double px,double py,const J&w){double ax=num(w,"x1"),ay=num(w,"y1"),ex=num(w,"x2")-ax,ey=num(w,"y2")-ay,l=ex*ex+ey*ey;
        double t=l==0?0:std::max(0.,std::min(1.,((px-ax)*ex+(py-ay)*ey)/l));
        return {ax+t*ex,ay+t*ey};
    }
    // Python JSON float repr uses fixed notation for exponents -4 through 15.
    inline std::string python_dump(const J&j){
        if(j.is_array()){std::string s="[";
            for(size_t i=0;i<j.size();++i){if(i)s+=",";
                s+=python_dump(j.at(i));
            }return s+"]";
        }
        if(j.is_object()){std::string s="{";
            bool first=true;
            for(auto&kv:j.as_object()){if(!first)s+=",";
                first=false;
                s+=J(kv.first).dump()+":"+python_dump(kv.second);
            }return s+"}";
        }
        if(!j.is_number()||j.is_int())return j.dump();
        std::string s=j.dump();
        auto e=s.find('e');
        if(e==std::string::npos)return s;
        int exponent=std::stoi(s.substr(e+1));
        if(exponent < -4||exponent>=16)return s;
        std::string mantissa=s.substr(0,e),sign;
        if(mantissa[0]=='-'){sign="-";
            mantissa.erase(0,1);
        }auto dot=mantissa.find('.');
        if(dot!=std::string::npos)mantissa.erase(dot,1);
        int point=exponent+1;
        if(point<=0)return sign+"0."+std::string(-point,'0')+mantissa;
        if(point>=int(mantissa.size()))return sign+mantissa+std::string(point-mantissa.size(),'0')+".0";
        return sign+mantissa.substr(0,point)+"."+mantissa.substr(point);

    }
    inline std::string sha256(const std::string&s){return sha2::hex(sha2::sha256_bytes(s));}
    struct StaleAnswer : std::runtime_error { using std::runtime_error::runtime_error;
    };

    class Engine {
        public:
        J rules, initial, players,bombs,broken,last_hit,kills,out_turn,out_tick,out_reason,invalid,program_errors,events;

        std::vector<int> standing;
        int turn=0,D=6,n=0,next_bomb=0,precision=4;
        bool done=false;
        double half=0;

        Engine(const J&submissions,const J&seed,const J&r):Engine(setup_header(submissions,seed,r)){}
        explicit Engine(const J&header):rules(header.at("rules")),initial(header),players(arr()),bombs(arr()),broken(arr()),last_hit(arr()),kills(arr()),out_turn(arr()),out_tick(arr()),out_reason(arr()),invalid(header.at("invalid")),program_errors(arr()),events(arr()){
            n=integer(header,"players");
            D=integer(rules,"decision_ticks");
            precision=integer(rules,"precision");
            half=num(rules.at("arena"),"half");

            for(int i=0;i<n;++i){auto&s=header.at("spawns").at(i);
                bool bad=!invalid.at(i).is_null();
                players.push_back(J{{"x",s.at(0)},{"y",s.at(1)},{"vx",0.0},{"vy",0.0},{"bar",1.0},{"charge",0.0},{"skill",nullptr},{"pause",0},{"out",bad},{"act",nullptr},{"act_ticks",0},{"aim",nullptr},{"bomb_cd",0},{"attack_cd",0},{"hammer_cd",0},{"held",0}});
                out_turn.push_back(bad?J(0):J());
                out_tick.push_back(bad?J(0):J());
                out_reason.push_back(bad?J("invalid"):J());
                last_hit.push_back(nullptr);
                kills.push_back(0);
                program_errors.push_back(0);
            }
            for(auto&w:initial.at("walls").as_array())standing.push_back(integer(w,"id"));
            done=alive().size()<=1;

        }
        double limit(int)const{return num(rules.at("time_limits"),turn==0?"first":"other");
        }
        std::string state_sha256()const{return sha256(python_dump(state()));
        }
        J decode(int seat,const std::string&line,J*notes=nullptr){J answer;
            try{answer=J::parse(line);
            }catch(...){throw std::runtime_error("not JSON");
            }return decode(seat,answer,notes);
        }
        J header()const{return initial;
        }
        static J direction(const J&v,const std::string&what){
            auto problem=[&](){return std::runtime_error(what+" must be [dx, dy], two finite numbers, or null");
            };

            if(v.is_null())return nullptr;
            if(!v.is_array()||v.size()!=2||!v.at(0).is_number()||!v.at(1).is_number())throw problem();

            auto convert=[&](const J&x){if(!x.is_int())return x.as_double();
                auto text=x.integer_decimal();
                char*end=nullptr;
                double d=std::strtod(text.c_str(),&end);
                if(!std::isfinite(d))throw problem();
                return d;
            };

            double x=convert(v.at(0)),y=convert(v.at(1));
            double big=std::max(std::abs(x),std::abs(y));
            if(big>1e150){x/=big;
                y/=big;
            }auto u=unit(x,y);
            return u?vec(u->first,u->second):J();

        }
        static J parse_order(const J&o,const J&r){
            if(o.is_null())return J::object{};
            if(!o.is_object())throw std::runtime_error("order must be an object or null");

            const std::set<std::string>keys{"move","charge","dash","hammer","bomb","attack"};
            for(auto&kv:o.as_object())if(!keys.count(kv.first))throw std::runtime_error("unknown order key: "+kv.first);

            J charge=has(o,"charge")?o.at("charge"):J();
            if(!charge.is_null()&&charge!=J("dash")&&charge!=J("hammer"))throw std::runtime_error("charge must be dash, hammer or null");

            J out{{"charge",charge}};
            for(auto k:{"move","dash","hammer","attack"})out[k]=direction(has(o,k)?o.at(k):J(),k);

            J b=has(o,"bomb")?o.at("bomb"):J();
            if(!b.is_null()){if(!b.is_object()||b.size()!=2||!b.contains("dir")||!b.contains("fuse"))throw std::runtime_error("bomb must contain dir and fuse");
                J dir=direction(b.at("dir"),"bomb dir");
                if(dir.is_null())throw std::runtime_error("bomb dir must not be [0, 0]");
                auto&f=b.at("fuse");
                if(!f.is_int())throw std::runtime_error("bomb fuse must be an integer in range");
                int value;
                try{value=f.get<int>();
                }catch(...){throw std::runtime_error("bomb fuse must be an integer in range");
                }if(value<integer(r.at("bomb"),"fuse_min")||value>integer(r.at("bomb"),"fuse_max"))throw std::runtime_error("bomb fuse must be an integer in range");
                b=J{{"dir",dir},{"fuse",value}};
            }out["bomb"]=b;
            return out;

        }
        J decode(int seat,const J&answer,J*notes=nullptr){if(!answer.is_object())throw std::runtime_error("the answer must be a JSON object");
            if(!answer.contains("turn")||!answer.at("turn").is_int())throw std::runtime_error("the answer has no integer turn");
            auto text=answer.at("turn").integer_decimal();
            int64_t at;
            try{at=answer.at("turn").as_int();
            }catch(...){if(text[0]=='-')throw StaleAnswer("answer to turn "+text);
                throw std::runtime_error("turn is not the current turn");
            }if(at<turn+1)throw StaleAnswer("answer to turn "+text);
            if(at!=turn+1)throw std::runtime_error("turn is not the current turn");
            if(has(answer,"error")){program_errors.at(seat)=program_errors.at(seat).get<int>()+1;
                if(notes){auto&e=answer.at("error");
                    std::string message=e.is_string()?e.as_string():e.dump();
                    (*notes)["program_error"]=message.substr(0,2000);
                }return J::object{};
            }return parse_order(has(answer,"order")?answer.at("order"):J(),rules);
        }
        J result()const{auto r=setup_ranking(initial.at("seed").integer_decimal(),turn,D,out_tick,kills);
            J rp=arr(),kp=arr(),score=arr();
            for(int i=0;i<n;++i){double rank=num(rules.at("score"),"rank_points")*(n-r.at("rank").at(i).get<int>()),kill=num(rules.at("score"),"kill_points")*kills.at(i).get<int>();
                bool ri=rules.at("score").at("rank_points").is_int(),ki=rules.at("score").at("kill_points").is_int();
                rp.push_back(ri?J(int64_t(rank)):J(rank));
                kp.push_back(ki?J(int64_t(kill)):J(kill));
                score.push_back(ri&&ki?J(int64_t(rank+kill)):J(rank+kill));
            }r["score"]=score;
            r["rank_points"]=rp;
            r["kill_points"]=kp;
            r["kills"]=kills;
            r["out_turn"]=out_turn;
            r["out_tick"]=out_tick;
            r["out_reason"]=out_reason;
            r["invalid"]=invalid;
            r["program_errors"]=program_errors;
            r["turns"]=turn;
            return r;
        }
        std::vector<int> alive()const{std::vector<int>a;
            for(int i=0;i<n;++i)if(!players.at(i).at("out").as_bool())a.push_back(i);
            return a;
        }
        J next()const{for(auto&p:initial.at("shrink").as_array())if(integer(p,"warn_turn")<=turn&&turn<integer(p,"end_turn"))return J{{"half",p.at("half")},{"turn",p.at("turn")},{"end_turn",p.at("end_turn")}};
            return nullptr;
        }
        J frame()const{J walls=arr();
            for(int id:standing)walls.push_back(id);
            return J{{"players",players},{"bombs",bombs},{"walls",walls},{"broken",broken},{"arena",J{{"half",half},{"next",next()}}},{"out",out_turn}};
        }
        J state()const{J walls=arr();
            for(int id:standing)walls.push_back(id);
            return J{{"turn",turn},{"done",done},{"half",half},{"players",players},{"bombs",bombs},{"next_bomb",next_bomb},{"last_hit",last_hit},{"kills",kills},{"out_turn",out_turn},{"out_tick",out_tick},{"out_reason",out_reason},{"invalid",invalid},{"program_errors",program_errors},{"walls",walls},{"broken",broken}};
        }
        void restore(const J&s){turn=integer(s,"turn");
            done=s.at("done").as_bool();
            half=num(s,"half");
            players=s.at("players");
            bombs=s.at("bombs");
            next_bomb=integer(s,"next_bomb");
            last_hit=s.at("last_hit");
            kills=s.at("kills");
            out_turn=s.at("out_turn");
            out_tick=s.at("out_tick");
            out_reason=s.at("out_reason");
            invalid=s.at("invalid");
            program_errors=s.at("program_errors");
            standing=s.at("walls").get<std::vector<int>>();
            broken=s.at("broken");
        }
        J observation(int seat)const{J w=arr(),b=broken,r=rules;
            r.erase("comment");
            r.erase("sandbox");
            for(int id:standing)w.push_back(initial.at("walls").at(id));
            for(auto&e:b.as_array())for(auto&kv:initial.at("walls").at(integer(e,"wall")).as_object())if(kv.first!="id")e[kv.first]=kv.second;
            return J{{"turn",turn+1},{"tick",turn*D},{"you",seat},{"n",n},{"turn_limit",limit(seat)},{"arena",J{{"half",half},{"next",next()}}},{"me",players.at(seat)},{"players",players},{"bombs",bombs},{"walls",w},{"broken",b},{"last_hit",last_hit},{"kills",kills},{"events",events},{"rules",r}};
        }
        void out(int seat,int tick,const char*kind,J by){auto&p=players.at(seat);
            p["out"]=true;
            p["vx"]=0.0;
            p["vy"]=0.0;
            p["act"]=nullptr;
            p["act_ticks"]=0;
            p["aim"]=nullptr;
            out_turn.at(seat)=turn+1;
            out_tick.at(seat)=tick;
            out_reason.at(seat)=kind;
            if(!by.is_null()){int b=by.get<int>();
                kills.at(b)=kills.at(b).get<int>()+1;
            }events.push_back(J{{"type","fall"},{"tick",tick},{"seat",seat},{"by",by},{"kind",kind}});
        }
        void throw_bomb(int seat,const J&o,int tick){auto&p=players.at(seat);
            auto&c=rules.at("bomb");
            double dx=o.at("dir").at(0).as_double(),dy=o.at("dir").at(1).as_double(),gap=num(rules.at("player"),"radius")+num(c,"radius")+num(c,"spawn_gap");
            bombs.push_back(J{{"id",next_bomb},{"x",num(p,"x")+dx*gap},{"y",num(p,"y")+dy*gap},{"vx",num(p,"vx")+dx*num(c,"throw_speed")},{"vy",num(p,"vy")+dy*num(c,"throw_speed")},{"fuse",o.at("fuse")},{"owner",seat}});
            events.push_back(J{{"type","bomb_throw"},{"tick",tick},{"seat",seat},{"id",next_bomb++},{"dir",vec(pyround(dx,precision),pyround(dy,precision))},{"fuse",o.at("fuse")}});
            p["bomb_cd"]=c.at("cooldown_ticks");
        }
        // Positions as at the tick's start. The hammer lands on release with its charge as scale; a jab after its windup.
        void swing(int seat,int tick,const std::vector<int>&a,bool hammer,double ax,double ay,double scale){auto&p=players.at(seat);
            auto&c=rules.at(hammer?"hammer":"attack");
            double reach=num(c,"reach"),gain=num(c,"power");
            J hits=arr(),struck=arr();
            auto hit=[&](J&q,int other){double dx=num(q,"x")-num(p,"x"),dy=num(q,"y")-num(p,"y");
                if(dx*dx+dy*dy>reach*reach)return;
                auto u=unit(dx,dy).value_or(std::make_pair(ax,ay));
                if(u.first*ax+u.second*ay<num(c,"cone_cos"))return;
                if(other<0){q["vx"]=num(q,"vx")+u.first*gain*scale/num(rules.at("bomb"),"mass");
                    q["vy"]=num(q,"vy")+u.second*gain*scale/num(rules.at("bomb"),"mass");
                    q["owner"]=seat;
                    struck.push_back(q.at("id"));
                }else{q["vx"]=num(q,"vx")+u.first*gain*scale;
                    q["vy"]=num(q,"vy")+u.second*gain*scale;
                    last_hit.at(other)=J::array{seat,tick};
                    hits.push_back(other);
                }};
            for(int i:a)if(i!=seat&&!players.at(i).at("out").as_bool())hit(players.at(i),i);
            for(auto&b:bombs.as_array())hit(b,-1);
            p["vx"]=num(p,"vx")-ax*num(c,"recoil")*scale;
            p["vy"]=num(p,"vy")-ay*num(c,"recoil")*scale;
            J e{{"type",hammer?"hammer":"attack"},{"tick",tick},{"seat",seat},{"dir",vec(pyround(ax,precision),pyround(ay,precision))},{"hits",hits},{"bombs",struck}};
            if(hammer)e["power"]=scale;
            events.push_back(e);
        }
        void act(int seat,const J&o,bool first,int tick,const std::vector<int>&a){auto&p=players.at(seat);
            auto&r=rules;

            for(auto k:{"attack_cd","hammer_cd"})p[k]=std::max(0,integer(p,k)-1);
            bool busy=has(p,"act");

            if(busy){p["act_ticks"]=integer(p,"act_ticks")-1;
                if(integer(p,"act_ticks")<=0){if(p.at("act")==J("jab")){
                        swing(seat,tick,a,false,p.at("aim").at(0).as_double(),p.at("aim").at(1).as_double(),1.);
                        p["act"]="jab_recover";
                        p["act_ticks"]=r.at("attack").at("recover_ticks");
                        p["aim"]=nullptr;
                    }if((p.at("act")==J("recover")||p.at("act")==J("jab_recover"))&&integer(p,"act_ticks")<=0){p["act"]=nullptr;
                        p["act_ticks"]=0;
                    }}}
            J want=has(o,"charge")?o.at("charge"):J(),skill=p.at("skill");
            if(want==J("hammer")&&integer(p,"hammer_cd")>0)want=nullptr;
            bool release=!busy&&first&&!skill.is_null()&&num(p,"charge")>0&&has(o,skill.as_string());
            if(!busy&&!release&&!want.is_null()&&!skill.is_null()&&skill!=want){p["charge"]=0.0;
                p["skill"]=nullptr;
            }
            bool jab=first&&!busy&&has(o,"attack")&&integer(p,"attack_cd")==0&&!(release&&skill==J("hammer"));
            bool charging=!busy&&!release&&!jab&&!want.is_null()&&num(p,"bar")>0;

            if(integer(p,"pause")>0)p["pause"]=integer(p,"pause")-1;
            else if(!charging)p["bar"]=std::max(num(p,"bar"),std::min(1.-num(p,"charge"),num(p,"bar")+num(r.at("bar"),"refill_rate")));

            if(release){auto&v=o.at(skill.as_string());
                double dx=v.at(0).as_double(),dy=v.at(1).as_double(),power=num(p,"charge");
                p["charge"]=0.0;
                p["skill"]=nullptr;
                p["pause"]=r.at("bar").at("pause_ticks");
                J e{{"tick",tick},{"seat",seat},{"dir",vec(pyround(dx,precision),pyround(dy,precision))},{"power",power}};
                if(skill==J("dash")){p["vx"]=num(p,"vx")+dx*num(r.at("dash"),"speed")*power;
                    p["vy"]=num(p,"vy")+dy*num(r.at("dash"),"speed")*power;
                    e["type"]="dash";
                    events.push_back(e);
                }else{p["hammer_cd"]=r.at("hammer").at("cooldown_ticks");
                    swing(seat,tick,a,true,dx,dy,power);
                    p["act"]="recover";
                    p["act_ticks"]=r.at("hammer").at("recover_ticks");
                    p["aim"]=nullptr;
                }
            }
            if(jab){auto&v=o.at("attack");
                p["act"]="jab";
                p["act_ticks"]=r.at("attack").at("windup_ticks");
                p["aim"]=v;
                p["attack_cd"]=r.at("attack").at("cooldown_ticks");
                events.push_back(J{{"type","attack_windup"},{"tick",tick},{"seat",seat},{"dir",vec(pyround(v.at(0).as_double(),precision),pyround(v.at(1).as_double(),precision))}});
            }
            if(charging){double amount=std::min(num(r.at("bar"),"charge_rate"),num(p,"bar"));
                p["bar"]=num(p,"bar")-amount;
                p["charge"]=num(p,"charge")+amount;
                p["skill"]=want;
            }
            if(num(p,"charge")<=0||charging)p["held"]=0;
            else{p["held"]=integer(p,"held")+1;
                if(integer(p,"held")>=integer(r.at("bar"),"overcharge_ticks")){events.push_back(J{{"type","overcharge"},{"tick",tick},{"seat",seat},{"skill",p.at("skill")},{"charge",p.at("charge")}});
                    p["charge"]=0.0;
                    p["skill"]=nullptr;
                    p["held"]=0;
                    p["pause"]=r.at("bar").at("pause_ticks");
                }}
            if(has(o,"move")&&!busy&&!has(p,"act")){double factor=p.at("skill")==J("dash")?num(r.at("move"),"charging_factor"):p.at("skill")==J("hammer")?num(r.at("hammer"),"charging_factor"):1.;
                p["vx"]=num(p,"vx")+o.at("move").at(0).as_double()*num(r.at("move"),"accel")*factor;
                p["vy"]=num(p,"vy")+o.at("move").at(1).as_double()*num(r.at("move"),"accel")*factor;
            }
            if(integer(p,"bomb_cd")>0)p["bomb_cd"]=integer(p,"bomb_cd")-1;
            else if(first&&has(o,"bomb")&&!busy&&!has(p,"act"))throw_bomb(seat,o.at("bomb"),tick);

        }
        void explode(const J&bomb,int tick,const std::vector<int>&a){auto&c=rules.at("bomb");
            double reach=num(c,"blast_radius");
            J hits=arr();
            auto push=[&](J&b,double mass){double dx=num(b,"x")-num(bomb,"x"),dy=num(b,"y")-num(bomb,"y"),d2=dx*dx+dy*dy;
                if(d2>reach*reach)return false;
                auto u=unit(dx,dy).value_or(std::make_pair(1.,0.));
                double gain=num(c,"blast_power")*(1-std::sqrt(d2)/reach)/mass;
                b["vx"]=num(b,"vx")+u.first*gain;
                b["vy"]=num(b,"vy")+u.second*gain;
                return true;
            };
            for(int s:a)if(!players.at(s).at("out").as_bool()&&push(players.at(s),1.)){last_hit.at(s)=J::array{bomb.at("owner"),tick};
                hits.push_back(s);
            }for(auto&b:bombs.as_array())push(b,num(c,"mass"));
            events.push_back(J{{"type","bomb_explode"},{"tick",tick},{"id",bomb.at("id")},{"owner",bomb.at("owner")},{"x",bomb.at("x")},{"y",bomb.at("y")},{"hits",hits}});

            auto ids=standing;
            for(int id:ids){auto&w=initial.at("walls").at(id);
                auto q=nearest(num(bomb,"x"),num(bomb,"y"),w);
                double dx=q.first-num(bomb,"x"),dy=q.second-num(bomb,"y");
                if(w.at("kind")==J("wall")&&dx*dx+dy*dy<=reach*reach){standing.erase(std::find(standing.begin(),standing.end(),id));
                    broken.push_back(J{{"wall",id},{"at",tick+integer(rules.at("walls"),"regen_ticks")}});
                    std::sort(broken.as_array().begin(),broken.as_array().end(),[](const J&a,const J&b){return integer(a,"wall")<integer(b,"wall");});
                    events.push_back(J{{"type","wall_break"},{"tick",tick},{"wall",id},{"bomb",bomb.at("id")}});
                }}
        }
        double contact(J&a,J&b,double reach,double ma,double mb){
            // Most bodies do not overlap. Avoid velocity lookups and eight JSON
            // writes when the reference contact routine would return immediately.
            double dx=num(b,"x")-num(a,"x"),dy=num(b,"y")-num(a,"y");
            if(dx*dx+dy*dy>=reach*reach)return 0.0;
            Body aa{num(a,"x"),num(a,"y"),num(a,"vx"),num(a,"vy")},bb{num(b,"x"),num(b,"y"),num(b,"vx"),num(b,"vy")};
            double impulse=pair(aa,bb,reach,ma,mb,num(rules.at("physics"),"restitution"));
            a["x"]=aa.x;
            a["y"]=aa.y;
            a["vx"]=aa.vx;
            a["vy"]=aa.vy;
            b["x"]=bb.x;
            b["y"]=bb.y;
            b["vx"]=bb.vx;
            b["vy"]=bb.vy;
            return impulse;
        }
        void bounce(J&b,double radius,const J&w,J who,int tick){double reach=radius+num(rules.at("walls"),"thickness")/2;
            auto q=nearest(num(b,"x"),num(b,"y"),w);
            double dx=num(b,"x")-q.first,dy=num(b,"y")-q.second,d2=dx*dx+dy*dy;
            if(d2>=reach*reach)return;
            std::pair<double,double>u;
            if(d2>0){double d=std::sqrt(d2);
                u={dx/d,dy/d};
            }else u=unit(-(num(w,"y2")-num(w,"y1")),num(w,"x2")-num(w,"x1")).value_or(std::make_pair(1.,0.));
            double nx=u.first,ny=u.second;
            b["x"]=q.first+nx*reach;
            b["y"]=q.second+ny*reach;
            double inward=num(b,"vx")*nx+num(b,"vy")*ny;
            if(inward>=0)return;
            double speed=w.at("kind")==J("wall")?num(rules.at("walls"),"restitution")*-inward:std::min(num(rules.at("spring"),"factor")*-inward,num(rules.at("spring"),"max_speed"));
            b["vx"]=num(b,"vx")+(speed-inward)*nx;
            b["vy"]=num(b,"vy")+(speed-inward)*ny;
            if(w.at("kind")==J("spring")&&speed>=num(rules.at("physics"),"min_impulse")){who["type"]="spring";
                who["tick"]=tick;
                who["wall"]=w.at("id");
                who["speed"]=pyround(speed,precision);
                events.push_back(who);
            }}
        void collide(const std::vector<int>&a,int tick){double sr=num(rules.at("player"),"radius"),br=num(rules.at("bomb"),"radius"),bm=num(rules.at("bomb"),"mass"),least=num(rules.at("physics"),"min_impulse");
            for(size_t k=0;k<a.size();++k)for(size_t l=k+1;l<a.size();++l){int i=a[k],j=a[l];
                double impulse=contact(players.at(i),players.at(j),sr+sr,1.,1.);
                if(impulse>=least){last_hit.at(i)=J::array{j,tick};
                    last_hit.at(j)=J::array{i,tick};
                    events.push_back(J{{"type","bump"},{"tick",tick},{"a",i},{"b",j},{"impulse",pyround(impulse,precision)}});
                }}
            for(int i:a){auto&p=players.at(i);
                for(auto&b:bombs.as_array()){double nx=num(b,"x")-num(p,"x"),ny=num(b,"y")-num(p,"y");
                    bool hitter=(num(p,"vx")+num(b,"vx"))*nx+(num(p,"vy")+num(b,"vy"))*ny>0;
                    if(contact(p,b,sr+br,1.,bm)>=least&&hitter)b["owner"]=i;
                }}for(size_t k=0;k<bombs.size();++k)for(size_t l=k+1;l<bombs.size();++l)contact(bombs.at(k),bombs.at(l),br+br,bm,bm);
            // Build wall bounds once for this tick. Outward nextafter expansion
            // keeps this a conservative rejection only; actual contacts still use
            // the exact reference closest-point test and original wall order.
            struct Bounds {const J*wall;double minx,maxx,miny,maxy;};
            std::vector<Bounds> bounds; bounds.reserve(standing.size());
            for(int id:standing){const auto&w=initial.at("walls").at(id);
                double x1=num(w,"x1"),y1=num(w,"y1");
                // Bound the actual floating-point interpolation, whose t=1
                // endpoint can differ from the stored endpoint after cancellation.
                double x2=x1+(num(w,"x2")-x1),y2=y1+(num(w,"y2")-y1);
                bounds.push_back({&w,std::min(x1,x2),std::max(x1,x2),std::min(y1,y2),std::max(y1,y2)});
            }
            auto bounce_all=[&](J&body,double radius,const char*key,J ident){
                double reach=radius+num(rules.at("walls"),"thickness")/2;
                for(const auto&b:bounds){double x=num(body,"x"),y=num(body,"y");
                    if(x<std::nextafter(b.minx-reach,-INFINITY)||x>std::nextafter(b.maxx+reach,INFINITY)||
                       y<std::nextafter(b.miny-reach,-INFINITY)||y>std::nextafter(b.maxy+reach,INFINITY))continue;
                    bounce(body,radius,*b.wall,J{{key,ident}},tick);
                }
            };
            for(int i:a)bounce_all(players.at(i),sr,"seat",i);
            for(auto&b:bombs.as_array())bounce_all(b,br,"bomb",b.at("id"));

        }
        void regrow(const J&w,const std::vector<int>&a,int tick){standing.push_back(integer(w,"id"));
            std::sort(standing.begin(),standing.end());
            J seats=arr(),bs=arr();
            auto push=[&](J&b,double radius){double reach=radius+num(rules.at("walls"),"thickness")/2;
                auto q=nearest(num(b,"x"),num(b,"y"),w);
                double dx=num(b,"x")-q.first,dy=num(b,"y")-q.second;
                if(dx*dx+dy*dy>=reach*reach)return false;
                auto u=unit(dx,dy).value_or(unit(-(num(w,"y2")-num(w,"y1")),num(w,"x2")-num(w,"x1")).value_or(std::make_pair(1.,0.)));
                b["x"]=q.first+u.first*reach;
                b["y"]=q.second+u.second*reach;
                double normal=num(b,"vx")*u.first+num(b,"vy")*u.second;
                b["vx"]=num(b,"vx")+(num(rules.at("walls"),"regen_push")-normal)*u.first;
                b["vy"]=num(b,"vy")+(num(rules.at("walls"),"regen_push")-normal)*u.second;
                return true;
            };
            for(int s:a)if(!players.at(s).at("out").as_bool()&&push(players.at(s),num(rules.at("player"),"radius")))seats.push_back(s);
            for(auto&b:bombs.as_array())if(push(b,num(rules.at("bomb"),"radius")))bs.push_back(b.at("id"));
            events.push_back(J{{"type","wall_back"},{"tick",tick},{"wall",w.at("id")},{"seats",seats},{"bombs",bs}});
        }
        J tick(const J&orders,bool first,int t){for(auto&p:initial.at("shrink").as_array()){int warn=(integer(p,"warn_turn")-1)*D+1,start=(integer(p,"turn")-1)*D+1,end=integer(p,"end_turn")*D;
                if(t==warn)events.push_back(J{{"type","shrink_warn"},{"tick",t},{"half",p.at("half")},{"turn",p.at("turn")},{"end_turn",p.at("end_turn")}});
                if(t==start)events.push_back(J{{"type","shrink"},{"tick",t},{"half",p.at("half")},{"from",p.at("from")},{"end_turn",p.at("end_turn")}});
                if(start<=t&&t<=end)half=pyround(num(p,"from")+(num(p,"half")-num(p,"from"))*(t-start+1)/(end-start+1),precision);
            }
            auto off=[&](const J&w){return std::max({std::abs(num(w,"x1")),std::abs(num(w,"y1")),std::abs(num(w,"x2")),std::abs(num(w,"y2"))})>half;
            };
            auto ids=standing;
            for(int id:ids)if(off(initial.at("walls").at(id))){standing.erase(std::find(standing.begin(),standing.end(),id));
                events.push_back(J{{"type","wall_fall"},{"tick",t},{"wall",id}});
            }auto a=alive();
            for(size_t k=0;k<broken.size();){J e=broken.at(k);
                auto&w=initial.at("walls").at(integer(e,"wall"));
                bool remove=false;
                if(off(w)){remove=true;
                    events.push_back(J{{"type","wall_fall"},{"tick",t},{"wall",w.at("id")}});
                }else if(t==integer(e,"at")-integer(rules.at("walls"),"warn_ticks"))events.push_back(J{{"type","wall_warn"},{"tick",t},{"wall",w.at("id")},{"at",e.at("at")}});
                else if(t>=integer(e,"at")){remove=true;
                    regrow(w,a,t);
                }if(remove)broken.as_array().erase(broken.as_array().begin()+k);
                else ++k;
            }
            for(size_t k=0;k<bombs.size();){auto&b=bombs.at(k);
                b["fuse"]=integer(b,"fuse")-1;
                if(integer(b,"fuse")<=0){J dead=b;
                    bombs.as_array().erase(bombs.as_array().begin()+k);
                    explode(dead,t,a);
                }else ++k;
            }
            for(int s:a)act(s,size_t(s)<orders.size()&&!orders.at(s).is_null()?orders.at(s):J::object{},first,t,a);

            for(int s:a){auto&b=players.at(s);
                b["x"]=num(b,"x")+num(b,"vx");
                b["y"]=num(b,"y")+num(b,"vy");
            }for(auto&b:bombs.as_array()){b["x"]=num(b,"x")+num(b,"vx");
                b["y"]=num(b,"y")+num(b,"vy");
            }collide(a,t);

            auto friction=[&](J&b,bool bomb){double slow=num(rules.at("physics"),"min_speed"),keep=1-num(rules.at(bomb?"bomb":"physics"),"friction"),vx=num(b,"vx"),vy=num(b,"vy");
                b["vx"]=vx*keep;
                b["vy"]=vy*keep;
                if(num(b,"vx")*num(b,"vx")+num(b,"vy")*num(b,"vy")<slow*slow){b["vx"]=0.;
                    b["vy"]=0.;
                }for(auto key:{"x","y","vx","vy","bar","charge"})if(b.contains(key))b[key]=pyround(num(b,key),precision)+0.;
                if(keep<1&&num(b,"vx")==vx)b["vx"]=0.;
                if(keep<1&&num(b,"vy")==vy)b["vy"]=0.;
            };
            for(int s:a)friction(players.at(s),false);
            for(auto&b:bombs.as_array())friction(b,true);

            for(int s:a){auto&p=players.at(s);
                if(std::max(std::abs(num(p,"x")),std::abs(num(p,"y")))>half){auto&h=last_hit.at(s);
                    if(!h.is_null()&&h.at(0).get<int>()!=s&&t-h.at(1).get<int>()<=integer(rules,"kill_credit_ticks"))out(s,t,"kill",h.at(0));
                    else out(s,t,"self",nullptr);
                }}
            J fallen=arr();
            for(size_t k=0;k<bombs.size();){auto&b=bombs.at(k);
                if(std::max(std::abs(num(b,"x")),std::abs(num(b,"y")))>half){fallen.push_back(b);
                    events.push_back(J{{"type","bomb_fall"},{"tick",t},{"id",b.at("id")}});
                    bombs.as_array().erase(bombs.as_array().begin()+k);
                }else ++k;
            }return fallen;

        }
        J step(const J&orders,const J&faults=J::array{}){events=arr();
            int start=turn*D;
            std::set<int> fs;
            for(auto&f:faults.as_array())fs.insert(f.get<int>());
            for(int s:fs)if(!players.at(s).at("out").as_bool())out(s,start+1,"fault",nullptr);
            J ticks=arr();
            for(int k=0;k<D;++k){J dropped=arr();
                std::set<int> fallen;
                if(alive().size()>1){dropped=tick(orders,k==0,start+k+1);
                    for(auto&e:events.as_array())if(e.at("type")==J("fall")&&e.at("kind")!=J("fault")&&integer(e,"tick")==start+k+1)fallen.insert(integer(e,"seat"));
                }J p=arr(),b=arr(),all=bombs;
                for(auto&d:dropped.as_array())all.push_back(d);
                std::sort(all.as_array().begin(),all.as_array().end(),[](const J&a,const J&b){return integer(a,"id")<integer(b,"id");});
                for(int s=0;s<n;++s){auto&q=players.at(s);
                    p.push_back(!q.at("out").as_bool()||fallen.count(s)?vec(num(q,"x"),num(q,"y")):J());
                }for(auto&q:all.as_array())b.push_back(J::array{q.at("id"),q.at("x"),q.at("y")});
                ticks.push_back(J{{"p",p},{"b",b}});
            }++turn;
            done=alive().size()<=1||turn>=integer(rules,"max_turns");
            J f=frame();
            f["turn"]=turn;
            f["events"]=events;
            f["ticks"]=ticks;
            return f;
        }
    };

} // namespace knockback
