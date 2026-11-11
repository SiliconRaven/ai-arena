// Experimental supervised variant; engine and record semantics remain shared via engine.hpp.
// Native local referee. Explicit trusted-local mode; Docker integration is not yet enabled.
#include "include/engine.hpp"
#include "include/supervised_seat.hpp"
#include <iostream>
#include <memory>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
using namespace smash;
int main(int argc,char**argv){
#ifdef _WIN32
 _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
#endif
 if(argc!=2||std::string(argv[1])!="--experimental-supervised"){std::cerr<<"Use --experimental-supervised through the trusted controller\n";return 2;}
#ifndef _WIN32
 signal(SIGPIPE,SIG_IGN);
#endif
 try{
  std::string line;std::getline(std::cin,line);J q=J::parse(line),submissions=arr();
  // A seat whose command is null has no program (it did not compile): its submission is the reason, and the engine
  // puts it out at turn 0 like any invalid submission. It is never asked, so it never needs a supervisor.
  const auto&commands=q.at("commands").as_array();const J noreasons=arr();const auto&reasons=q.contains("invalid")?q.at("invalid").as_array():noreasons.as_array();
  for(size_t i=0;i<commands.size();++i){
   if(commands[i].is_null()){if(i>=reasons.size()||!reasons[i].is_string())throw std::runtime_error("a seat without a command needs its invalid reason");submissions.push_back(reasons[i]);}
   else submissions.push_back(J{{"program","native-command"}});}
  Engine game(submissions,q.at("seed"),q.at("rules"));
  for(auto&cmd:commands){if(cmd.is_null())continue;auto a=cmd.get<std::vector<std::string>>();if(a.empty()||a[0]!=q.at("supervisor").as_string())throw std::runtime_error("unverified supervisor command");}
  std::vector<std::unique_ptr<SupervisedSeat>>bots;for(auto&cmd:commands)bots.push_back(cmd.is_null()?nullptr:std::make_unique<SupervisedSeat>(cmd.get<std::vector<std::string>>()));
  J header=game.header();header["submissions"]=submissions;header["schema_version"]="sm-native-records-1";header["sandbox"]="supervised-experimental";header["observation_encoding"]="sorted-json";header["state_sha256"]=game.state_sha256();
  if(q.contains("names"))header["names"]=q.at("names");
  J frames=arr(),records=arr(),timing=arr(),fault_history=arr(),barriers=arr();J first=game.frame();first["turn"]=0;first["events"]=arr();frames.push_back(first);
  std::vector<J>worker_comms(game.n,J::object{});
  std::vector<int>strikes(game.n,0);for(int i=0;i<game.n;++i)fault_history.push_back(arr());
  auto epoch=Clock::now();size_t missing_answers=0,missing_usage=0,not_sent=0;
  while(!game.done){
   auto barrier_start=Clock::now();
   auto ready_deadline=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(num(game.rules.at("sandbox"),"start_timeout")));
   for(int i:game.alive())bots[i]->ready(game.turn+1,ready_deadline);
   barriers.push_back(J{{"turn",game.turn+1},{"ms",std::chrono::duration<double,std::milli>(Clock::now()-barrier_start).count()}});

   auto asked=game.alive();std::vector<std::string>observations(game.n);std::vector<Clock::time_point>starts(game.n);std::vector<bool>sent(game.n,false);
   J orders=arr(),entries=arr(),times=arr(),faults=arr();for(int i=0;i<game.n;++i){orders.push_back(J::object{});entries.push_back(nullptr);times.push_back(nullptr);}
   for(int i:asked)observations[i]=game.observation(i).dump()+"\n";
   for(int i:asked){starts[i]=Clock::now();sent[i]=bots[i]->transport().send(observations[i]);entries.at(i)=J{{"obs_sha256",sha256(observations[i])},{"obs_bytes",observations[i].size()},{"sent",bool(sent[i])}};if(!sent[i])++not_sent;}
   for(int i:asked){
    auto deadline=starts[i]+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(game.limit(i)));
    std::string problem,raw;J notes=J::object{};int stale=0;std::optional<Answer>answer;
    for(;;){answer=bots[i]->transport().receive(deadline);if(!answer){problem=bots[i]->transport().exited()?"exited":"timeout";break;}if(answer->eof){problem="exited";break;}if(answer->arrived>deadline){problem="timeout";break;}if(answer->too_long){problem="bad_answer: a line over 65536 bytes";break;}
     raw=answer->text;try{auto supervised=J::parse(raw);if(supervised.contains("worker_comms")&&supervised.at("worker_comms").is_object())worker_comms[i]=supervised.at("worker_comms");if(supervised.contains("supervisor_fault")){auto declared=supervised.at("supervisor_fault").as_string();if(declared=="exited"||declared.rfind("bad_answer: ",0)==0)problem=declared;else throw std::runtime_error("invalid supervisor fault");}else orders.at(i)=game.decode(i,raw,&notes);}catch(const StaleAnswer&){++stale;answer.reset();continue;}catch(const std::exception&e){problem=std::string("bad_answer: ")+e.what();}break;
    }
    auto&entry=entries.at(i);entry["orders"]=orders.at(i);entry["status"]=problem.empty()?"ok":problem.substr(0,problem.find(':'));if(!problem.empty()){entry["problem"]=problem.substr(0,300);++missing_answers;}
    if(!raw.empty())entry["raw"]=raw;if(stale)entry["stale"]=stale;
    bool usage=false;if(problem.empty()&&!raw.empty()){auto parsed=J::parse(raw);if(parsed.contains("error")&&!parsed.at("error").is_null())entry["error"]=parsed.at("error");if(parsed.contains("usage")){entry["usage"]=parsed.at("usage");usage=true;}}
    if(!usage)++missing_usage;
    J arrived=nullptr;if(answer&&!answer->eof)arrived=std::chrono::duration<double>(answer->arrived-epoch).count();
    times.at(i)=J{{"sent",std::chrono::duration<double>(starts[i]-epoch).count()},{"arrived",arrived},{"limit",game.limit(i)},{"on_time",answer.has_value()&&!answer->eof&&answer->arrived<=deadline}};
    if(problem.empty())strikes[i]=0;else{++strikes[i];fault_history.at(i).push_back(J{{"turn",game.turn+1},{"problem",problem}});if(strikes[i]>=integer(game.rules,"max_strikes")||problem=="exited")faults.push_back(i);}
   }
   timing.push_back(J{{"turn",game.turn+1},{"seats",times}});frames.push_back(game.step(orders,faults));records.push_back(J{{"turn",game.turn},{"seats",entries},{"faults",faults},{"state_sha256",game.state_sha256()}});
  }
  // Supervisor transport counters plus the container worker losses the supervisor reports.
  auto worker_count=[&](int seat,const char*key)->size_t{const J&w=worker_comms[seat];if(!w.contains(key)||!w.at(key).is_number_integer())return 0;try{auto n=w.at(key).as_int();return n>0?size_t(n):0;}catch(const std::exception&){return 0;}};
  J comms=arr();size_t dropped=0,stderr_total=0;for(int seat=0;seat<game.n;++seat){
   if(!bots[seat]){comms.push_back(J{{"dropped_lines",0},{"long_lines",0},{"unsent_observations",0},{"stderr_dropped_bytes",0}});continue;}
   auto&t=bots[seat]->transport();
   size_t lost=t.dropped_lines.load()+worker_count(seat,"dropped_lines"),longs=t.long_lines.load()+worker_count(seat,"long_lines"),err=t.stderr_bytes.load()+worker_count(seat,"stderr_bytes");
   dropped+=lost+longs;stderr_total+=err;comms.push_back(J{{"dropped_lines",lost},{"long_lines",longs},{"unsent_observations",t.unsent.load()},{"stderr_dropped_bytes",err}});}
  J result=game.result();result["faults"]=fault_history;J integrity{{"truncated",dropped>0||stderr_total>0},{"missing_records",J{{"answer",missing_answers},{"usage",missing_usage},{"observation_not_sent",not_sent}}}};
  std::cout<<J{{"header",header},{"frames",frames},{"result",result},{"records",records},{"timing",timing},{"comms",comms},{"integrity",integrity},{"supervisor_barriers",barriers}}.dump()<<'\n';
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
