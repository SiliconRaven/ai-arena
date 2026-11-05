// Host-side experimental native supervisor. Docker lifecycle stays explicitly opt-in.
#include "include/json.hpp"
#include "include/process.hpp"
#include <iostream>
#include <memory>
#include <regex>
#include <cmath>
#include <fstream>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>
#endif
using J=mini_json::Json;
using namespace knockback;
struct WorkerExit:std::runtime_error{using std::runtime_error::runtime_error;};
struct WorkerProtocol:std::runtime_error{using std::runtime_error::runtime_error;};
struct StepError:std::runtime_error{using std::runtime_error::runtime_error;};
static std::string bounded_text(std::string text,size_t limit){if(text.size()<=limit)return text;text.resize(limit);for(;;){try{J(text).dump();return text;}catch(const std::exception&){if(text.empty())return text;text.pop_back();}}}
static constexpr size_t MAX_MEMORY=32768,MAX_REQUEST=2*1024*1024;
static void kill_container(const std::string&name){
 Process kill({"docker","kill",name});
 auto deadline=Clock::now()+std::chrono::seconds(10);
 for(;;){auto line=kill.receive(deadline);if(!line)throw std::runtime_error("container kill timed out; execution termination unverified");if(line->eof)break;}
 // A stopped/not-running container may make docker kill fail; verify state independently.
 Process inspect({"docker","inspect","--format","{{.State.Running}}",name});
 auto answer=inspect.receive(Clock::now()+std::chrono::seconds(10));
 if(!answer||answer->text!="false")throw std::runtime_error("container is still running or cannot be inspected");
}
// The worker container's memory high-water mark, measured by the host, never claimed by the worker. Where the
// container's cgroup is on this host (a native Linux Docker engine) its memory.peak is read on every turn; otherwise
// (Docker Desktop: the cgroup lives in its VM) a background thread samples it once a second with `docker exec ... cat`,
// off the turn's clock. The value is the highest seen in this match (a restart starts a new cgroup lifetime).
struct MemoryProbe{
 std::string id,host;std::atomic<long long>peak{-1};std::atomic<bool>stop{false},sampling{false};std::thread sampler;
 explicit MemoryProbe(std::string container):id(std::move(container)){}
 static long long number(const std::string&path){std::ifstream in(path);long long n;return in>>n?n:-1;}
 void note(long long n){long long seen=peak.load();while(n>seen&&!peak.compare_exchange_weak(seen,n)){}}
 void exec_sample(){Process p({"docker","exec",id,"cat","/sys/fs/cgroup/memory.peak"});auto line=p.receive(Clock::now()+std::chrono::seconds(3));
  if(line&&!line->eof&&!line->too_long){try{note(std::stoll(line->text));}catch(const std::exception&){}}}
 void turn(){
  if(host.empty())for(auto path:{"/sys/fs/cgroup/system.slice/docker-"+id+".scope/memory.peak","/sys/fs/cgroup/docker/"+id+"/memory.peak"})
   if(number(path)>=0){host=path;break;}
  if(!host.empty()){note(number(host));return;}
  if(!sampling.exchange(true))sampler=std::thread([this]{while(!stop){exec_sample();for(int i=0;i<10&&!stop;++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));}});
 }
 J kb()const{long long v=peak.load();return v<0?J(nullptr):J(v/1024);}
 const char*source()const{return host.empty()?"docker-exec-sampled":"host-cgroup";}
 ~MemoryProbe(){stop=true;if(sampler.joinable())sampler.join();}
};
struct ContainerGuard{std::string name;~ContainerGuard(){if(!name.empty()){try{kill_container(name);}catch(const std::exception&e){std::cerr<<"container cleanup unverified: "<<name<<": "<<e.what()<<'\n';}}}};
static bool bounded_line(std::string&line){line.clear();char c;while(std::cin.get(c)){if(c=='\n')return true;if(line.size()>=MAX_REQUEST)throw std::runtime_error("observation exceeds supervisor input bound");line+=c;}return !line.empty();}
int main(int argc,char**argv){
#ifdef _WIN32
 _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
 int wide_argc=0;auto wide_argv=CommandLineToArgvW(GetCommandLineW(),&wide_argc);if(!wide_argv)return 2;
 std::vector<std::string> arguments;for(int i=0;i<wide_argc;++i)arguments.push_back(utf8(wide_argv[i]));LocalFree(wide_argv);
 std::vector<char*> arg_ptrs;for(auto&arg:arguments)arg_ptrs.push_back(arg.data());argc=wide_argc;argv=arg_ptrs.data();
 bool control_stdout=argc>=2&&std::string(argv[1])=="--control-stdout";if(control_stdout){--argc;++argv;}
 int control=-1;
#else
 signal(SIGPIPE,SIG_IGN);
 int control=-1;
 if(argc>=3&&std::string(argv[1])=="--control-fd"){control=std::stoi(argv[2]);argc-=2;argv+=2;if(control<3||fcntl(control,F_SETFD,FD_CLOEXEC)<0)return 2;}
#endif
 try{
  bool managed_gated=argc==5&&std::string(argv[1])=="--managed-step-docker";
  bool managed=managed_gated||(argc==5&&std::string(argv[1])=="--managed-docker");
  bool docker=managed||(argc==3&&std::string(argv[1])=="--experimental-docker");
  bool local_gated=argc>=3&&std::string(argv[1])=="--trusted-local-gated";
  bool local_ready=local_gated||(argc>=3&&std::string(argv[1])=="--trusted-local-ready");
  bool local=local_ready||(argc>=3&&std::string(argv[1])=="--trusted-local");
  if(!docker&&!local)throw std::runtime_error("use --experimental-docker MANAGED_CONTAINER or --trusted-local EXECUTABLE [ARGS]");
  std::string name=docker?argv[2]:"";std::vector<std::string>cmd;
  if(docker){if(!std::regex_match(name,std::regex(managed?"[0-9a-f]{64}":"kb-lab-native-[a-z0-9-]{1,48}")))throw std::runtime_error("invalid managed container name");cmd={"docker","start","--attach","--interactive",name};}
  else for(int i=2;i<argc;++i)cmd.push_back(argv[i]);
  auto verify=[&]{if(managed){
   if(!std::regex_match(argv[3],std::regex("sha256:[0-9a-f]{64}"))||!std::regex_match(argv[4],std::regex("[0-9a-f]{32}:[0-9a-f]{32}")))throw std::runtime_error("invalid provenance binding");
   Process inspect({"docker","inspect","--format","{{json .}}",name});auto result=inspect.receive(Clock::now()+std::chrono::seconds(10));
   if(!result||result->eof||result->too_long)throw std::runtime_error("container provenance inspect failed");
   J state=J::parse(result->text);
   if(state.at("Id")!=J(name)||state.at("Image")!=J(argv[3])||state.at("Config").at("Labels").at("org.kb-lab-native.owner")!=J(argv[4]))throw std::runtime_error("container provenance mismatch");
   const J&host=state.at("HostConfig");
   if(host.at("NetworkMode")!=J("none")||host.at("ReadonlyRootfs")!=J(true)||host.at("Privileged")!=J(false)||state.at("Config").at("User")!=J("65534:65534")||host.value("PidMode","")!="")throw std::runtime_error("container security boundary mismatch");
   bool caps=false,privileges=false;
   for(auto&value:host.at("CapDrop").as_array())if(value==J("ALL")||value==J("all"))caps=true;
   for(auto&value:host.at("SecurityOpt").as_array())if(value==J("no-new-privileges")||value==J("no-new-privileges:true"))privileges=true;else throw std::runtime_error("unexpected security override");
   const auto&mounts=state.at("Mounts").as_array();
   if(!caps||!privileges||host.at("Memory").as_int()<=0||host.at("MemorySwap")!=host.at("Memory")||host.at("PidsLimit").as_int()<=0||host.at("NanoCpus").as_int()<=0||mounts.size()!=1||mounts[0].at("Type")!=J("bind")||mounts[0].at("Destination")!=J("/bot")||mounts[0].at("RW")!=J(false))throw std::runtime_error("container resources or mount boundary mismatch");
  }};
  verify();
  ContainerGuard guard;
  std::unique_ptr<MemoryProbe>memory_probe;if(managed)memory_probe=std::make_unique<MemoryProbe>(name);
  std::unique_ptr<Process>worker;J memory=nullptr;std::string line;long long sequence=0;bool launch_pending=false;
  // Worker transport losses survive restarts so the judge can report them in comms.
  size_t lost_lines=0,long_lines=0,stderr_bytes=0;bool exited_pending=false;
  auto retire_worker=[&]{if(worker){lost_lines+=worker->dropped_lines.load();long_lines+=worker->long_lines.load();stderr_bytes+=worker->stderr_bytes.load();worker.reset();}};
  auto worker_comms=[&]{return J{{"dropped_lines",lost_lines+(worker?worker->dropped_lines.load():0)},{"long_lines",long_lines+(worker?worker->long_lines.load():0)},{"stderr_bytes",stderr_bytes+(worker?worker->stderr_bytes.load():0)}};};
  auto start=[&]{launch_pending=managed_gated||local_gated;verify();worker=std::make_unique<Process>(cmd);if(docker||local_ready){auto marker=worker->receive(Clock::now()+std::chrono::seconds(60));if(!marker||marker->text!="#kb-native-ready")throw std::runtime_error("worker readiness failed");}};
  if(docker)guard.name=name;
  start();
  auto ready=[&]{
#ifdef _WIN32
   if(control_stdout)std::cout<<J{{"protocol","kb-supervisor-control-1"},{"next",sequence+1}}.dump()<<std::endl;
#else
   if(control>=0){auto msg=J{{"protocol","kb-supervisor-control-1"},{"next",sequence+1}}.dump()+"\n";size_t offset=0;while(offset<msg.size()){auto n=write(control,msg.data()+offset,msg.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("supervisor control channel closed");offset+=size_t(n);}}
#endif
  };
  ready();
  while(bounded_line(line)){
   if(line.size()>MAX_REQUEST)throw std::runtime_error("observation exceeds supervisor input bound");
   J obs=J::parse(line);double allowed=obs.at("turn_limit").as_double()*obs.at("rules").value("program_time_share",1.0);
   if(exited_pending){   // found gone when it was to be restarted: the exit fault, on this next turn
    J response{{"turn",obs.at("turn")},{"order",nullptr},{"error",nullptr},{"supervisor_fault","exited"},
               {"usage",J{{"ms",0.0},{"share_ms",std::round(allowed*1e6)/1000},{"mem",nullptr},{"peak_kb",nullptr},
                          {"cgroup_peak_kb",memory_probe?memory_probe->kb():J(nullptr)}}},{"worker_comms",worker_comms()}};
    std::cout<<response.dump()<<std::endl;break;
   }
   if(!std::isfinite(allowed)||allowed<=0)throw std::runtime_error("invalid time share");
   auto began=Clock::now(),deadline=began+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(allowed));
   J order=nullptr,error=nullptr,mem=nullptr,debug=nullptr,pending_memory=nullptr;bool restart=false,commit_memory=false,worker_gone=false;std::string fault;
   try{
    // Every request carries authoritative committed state, never an internal worker checkpoint.
    auto request=J{{"protocol","kb-step-json-1"},{"sequence",++sequence},{"observation",obs},{"memory",memory}}.dump()+"\n";
    if(launch_pending){request="#kb-step-launch\n"+request;launch_pending=false;}
    if(request.size()>MAX_REQUEST||!worker->send(request))throw std::runtime_error("worker request exceeds bound or is unavailable");
    auto answer=worker->receive(deadline);
    if(!answer||answer->arrived>deadline)throw std::runtime_error("over its time share");
    if(answer->eof)throw WorkerExit("worker exited");
    if(answer->too_long)throw WorkerProtocol("worker answer exceeds 65536 bytes");
    J packet;try{packet=J::parse(answer->text);}catch(const std::exception&){throw WorkerProtocol("invalid worker JSON");}
    if(!packet.is_object()||!packet.contains("protocol")||!packet.contains("sequence")||!packet.contains("order")||!packet.contains("error"))throw WorkerProtocol("missing worker protocol fields");
    if(packet.at("protocol")!=J("kb-step-json-1")||packet.at("sequence")!=J(sequence))throw WorkerProtocol("wrong worker protocol or sequence");
    if(!packet.at("error").is_null()){if(!packet.at("error").is_string())throw WorkerProtocol("invalid worker error type");throw StepError(packet.at("error").as_string().empty()?"worker reported an empty error":bounded_text(packet.at("error").as_string(),300));}
    if(!packet.contains("memory"))throw WorkerProtocol("missing worker memory");
    if(!packet.at("order").is_null()&&!packet.at("order").is_object())throw StepError("invalid worker order");
    auto snapshot=packet.at("memory").dump();if(snapshot.size()>MAX_MEMORY)throw StepError("worker JSON memory exceeds 32768 bytes");
    if(packet.contains("debug")&&packet.at("debug").is_string()){auto text=packet.at("debug").as_string();auto kept=bounded_text(text,2000);debug=J{{"program","bot.cpp"},{"phase","step"},{"text",kept},{"dropped",text.size()-kept.size()}};}
    if(Clock::now()>deadline)throw std::runtime_error("over its time share during validation");
    pending_memory=packet.at("memory");order=packet.at("order");mem=snapshot.size();commit_memory=true;
   }catch(const WorkerExit&e){restart=true;worker_gone=true;error="worker exited";}catch(const WorkerProtocol&e){restart=true;fault=std::string("bad_answer: ")+e.what();}catch(const StepError&e){error=std::string(e.what()).substr(0,300);}catch(const std::exception&e){restart=true;error=std::string(e.what()).substr(0,300);}
   auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-began).count();
   if(memory_probe)memory_probe->turn();
   J response{{"turn",obs.at("turn")},{"order",order},{"error",error},{"usage",J{{"ms",std::round(elapsed*1000)/1000},{"share_ms",std::round(allowed*1e6)/1000},{"mem",mem},{"peak_kb",nullptr},{"cgroup_peak_kb",memory_probe?memory_probe->kb():J(nullptr)}}}};
   if(memory_probe)response["usage"]["memory_source"]=memory_probe->source();
   if(!debug.is_null())response["native_debug"]=debug;
   if(!fault.empty())response["supervisor_fault"]=fault;
   response["worker_comms"]=worker_comms();
   auto encoded=response.dump();if(encoded.size()+1>65536){commit_memory=false;response.erase("native_debug");response["order"]=nullptr;response["error"]="supervisor answer exceeds line bound";response["usage"]["mem"]=nullptr;encoded=response.dump();}
   // Commit only after the final accepted envelope has serialized within its bound.
   // Rejected answers must not advance the authoritative checkpoint or report new memory.
   if(commit_memory)memory=std::move(pending_memory);
   std::cout<<encoded<<std::endl;
   if(fault=="exited")break;
   if(restart){
    // A worker that exited by itself (crash, exit, OOM) is not restarted. Whether its end was seen within the share
    // (end of output) or only now (the engine's attach latency made it a timeout), the turn is a program error and the
    // next turn the exit fault, so the outcome does not depend on the engine. SIGKILL from `docker kill` leaves exit
    // code 137 without OOMKilled; anything else is the worker's own.
    bool own_exit=worker_gone;
    if(docker&&!own_exit){kill_container(name);
     Process inspect({"docker","inspect","--format","{{.State.ExitCode}} {{.State.OOMKilled}}",name});
     auto state=inspect.receive(Clock::now()+std::chrono::seconds(10));
     own_exit=state&&!state->eof&&state->text!="137 false";}
    if(own_exit)exited_pending=true;
    else{retire_worker();start();}
   }
   ready();
  }
  if(docker){kill_container(name);guard.name.clear();}
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
