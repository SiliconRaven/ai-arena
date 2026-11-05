#pragma once
// Opt-in cooperative native SDK. POSIX process isolation is not a security sandbox.
#include "json.hpp"
#include <chrono>
#include <iostream>
#include <fstream>
#include <utility>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
namespace knockback_sdk {
using J=mini_json::Json;
using Step=std::pair<J,J>(*)(const J&,J&);
using Clock=std::chrono::steady_clock;
constexpr size_t MAX_MEMORY=16*1024*1024, MAX_DEBUG=2000, MAX_ANSWER=65536;
inline bool write_all(int fd,const std::string&s){size_t n=0;while(n<s.size()){ssize_t k=write(fd,s.data()+n,s.size()-n);if(k<0&&errno==EINTR)continue;if(k<=0)return false;n+=size_t(k);}return true;}
inline std::string ascii_debug(const std::string&s){std::string out;for(unsigned char c:s){if(c<128)out+=char(c);else{const char*h="0123456789abcdef";out+="\\x";out+=h[c>>4];out+=h[c&15];}}return out;}
inline J cgroup_peak(){std::ifstream in("/sys/fs/cgroup/memory.peak");long long n;if(in>>n)return n/1024;return nullptr;}
struct Loader {
 J memory=nullptr;
 long long peak=0;
 J run(const J&obs,Step step){
  auto start=Clock::now();double allowed=obs.at("turn_limit").as_double()*obs.at("rules").value("program_time_share",1.0);
  if(!std::isfinite(allowed)||allowed<=0)throw std::runtime_error("invalid time share");
  int result[2],debug[2];if(pipe(result))throw std::runtime_error("result pipe failed");if(pipe(debug)){close(result[0]);close(result[1]);throw std::runtime_error("debug pipe failed");}
  std::cout.flush();std::cerr.flush();fflush(nullptr);
  pid_t pid=fork();if(pid<0){for(int fd:result)close(fd);for(int fd:debug)close(fd);throw std::runtime_error("fork failed");}
  if(pid==0){
#ifdef __linux__
   prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(125);
#endif
   setpgid(0,0);close(result[0]);close(debug[0]);dup2(debug[1],1);dup2(debug[1],2);close(debug[1]);int empty=open("/dev/null",O_RDONLY);dup2(empty,0);close(empty);
   J packet;
   try{J local=memory;auto answer=step(obs,local);if(!answer.first.is_null()&&!answer.first.is_object())throw std::runtime_error("order must be an object or null");std::string snapshot=answer.second.dump();if(snapshot.size()>MAX_MEMORY)throw std::runtime_error("native JSON memory exceeds 16 MiB");packet=J{{"order",answer.first},{"memory",answer.second},{"error",nullptr}};}
   catch(const std::exception&e){packet=J{{"order",nullptr},{"error",std::string(e.what()).substr(0,300)}};}
   catch(...){packet=J{{"order",nullptr},{"error","native step raised an unknown exception"}};}
   try{auto data=packet.dump();if(data.size()>MAX_MEMORY+MAX_ANSWER)throw std::runtime_error("native result exceeds bound");write_all(result[1],data);}catch(...){write_all(result[1],"{\"order\":null,\"error\":\"native result serialization failed\"}");}
   std::cout.flush();std::cerr.flush();fflush(nullptr);close(result[1]);_exit(0);
  }
  setpgid(pid,pid);close(result[1]);close(debug[1]);fcntl(result[0],F_SETFL,O_NONBLOCK);fcntl(debug[0],F_SETFL,O_NONBLOCK);
  std::string data,logs;size_t dropped=0;bool overflow=false,done=false,overtime=false;int status=0;struct rusage resources{};
  auto drain=[&](int fd,std::string&out,size_t cap,bool is_debug){char buf[8192];for(int rounds=0;rounds<16;++rounds){ssize_t n=read(fd,buf,sizeof buf);if(n<=0)break;size_t keep=std::min(size_t(n),cap-out.size());out.append(buf,keep);if(is_debug)dropped+=size_t(n)-keep;else if(keep<size_t(n))overflow=true;}};
  while(!done){
   drain(result[0],data,MAX_MEMORY+MAX_ANSWER,false);drain(debug[0],logs,MAX_DEBUG,true);
   if(wait4(pid,&status,WNOHANG,&resources)==pid){done=true;break;}
   double remaining=allowed-std::chrono::duration<double>(Clock::now()-start).count();
   if(remaining<=0||overflow){overtime=remaining<=0;break;}
   pollfd fds[2]={{result[0],POLLIN,0},{debug[0],POLLIN,0}};poll(fds,2,std::max(1,std::min(2,int(remaining*1000))));
  }
  kill(-pid,SIGKILL); // reap any step descendants, including those retaining output pipes
  if(!done){kill(pid,SIGKILL);while(wait4(pid,&status,0,&resources)<0&&errno==EINTR){}}
  drain(result[0],data,MAX_MEMORY+MAX_ANSWER,false);drain(debug[0],logs,MAX_DEBUG,true);close(result[0]);close(debug[0]);
  peak=std::max(peak,static_cast<long long>(resources.ru_maxrss));
  J order=nullptr,error=nullptr,mem=nullptr;
  try{
   if(overtime)throw std::runtime_error("over its time share ("+std::to_string(std::llround(allowed*1000))+" ms)");
   if(overflow)throw std::runtime_error("native result exceeds bound");
   if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("native step exited abnormally");
   J packet=J::parse(data);error=packet.at("error");
   if(error.is_null()){
    auto serialized=packet.at("memory").dump();
    if(std::chrono::duration<double>(Clock::now()-start).count()>allowed)throw std::runtime_error("over its time share");
    memory=packet.at("memory");mem=serialized.size();order=packet.at("order");
   }
  }catch(const std::exception&e){error=std::string(e.what()).substr(0,300);}
  double spent=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  J usage{{"ms",std::round(spent*1000)/1000},{"peak_kb",peak},{"cgroup_peak_kb",cgroup_peak()},{"mem",mem},{"share_ms",std::round(allowed*1e6)/1000}};
  if(!logs.empty()||dropped){J debugline{{"turn",obs.at("turn")},{"program","bot.cpp"},{"phase","step"},{"text",ascii_debug(logs)},{"dropped",dropped}};std::cerr<<"#kb-lab-debug "<<debugline.dump()<<'\n';}
  return J{{"turn",obs.at("turn")},{"order",order},{"error",error},{"usage",usage},
   {"native_debug",J{{"program","bot.cpp"},{"phase","step"},{"text",ascii_debug(logs)},{"dropped",dropped}}}};
 }
};
inline int main(Step step){signal(SIGPIPE,SIG_IGN);Loader loader;std::string line;while(std::getline(std::cin,line)){try{J answer=loader.run(J::parse(line),step);std::string data=answer.dump();if(data.size()+1>MAX_ANSWER){answer.erase("native_debug");data=answer.dump();}if(data.size()+1>MAX_ANSWER){answer["usage"]=nullptr;data=answer.dump();}if(data.size()+1>MAX_ANSWER){answer["order"]=nullptr;answer["error"]="the order is over the answer line limit";data=answer.dump();}std::cout<<data<<std::endl;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}return 0;}
}
