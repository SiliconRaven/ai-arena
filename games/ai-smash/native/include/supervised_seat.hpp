#pragma once
#include "process.hpp"
#include "json.hpp"
#ifndef _WIN32
#include <poll.h>
#endif
#include <memory>
namespace smash {
// Dedicated supervisor -> judge pipe. Worker stdout never enters this channel.
#ifdef _WIN32
class SupervisedSeat {
 std::unique_ptr<Process> process;
public:
 explicit SupervisedSeat(std::vector<std::string> command){command.insert(command.begin()+1,"--control-stdout");process=std::make_unique<Process>(command);}
 void ready(long long sequence,Clock::time_point deadline){
  // Control and answers share stdout here: an answer the judge stopped waiting for arrives first and is dropped.
  mini_json::Json packet;
  for(;;){auto line=process->receive(deadline);if(!line||line->eof||line->too_long)throw std::runtime_error("supervisor readiness failed");
   packet=mini_json::Json::parse(line->text);if(packet.contains("protocol"))break;}
  if(packet.at("protocol")!=mini_json::Json("sm-supervisor-control-1")||packet.at("next")!=mini_json::Json(sequence))throw std::runtime_error("invalid supervisor readiness state");
 }
 Process* operator->(){return process.get();}
 Process& transport(){return *process;}
};
#else
class SupervisedSeat {
 int control=-1;std::unique_ptr<Process>process;std::string pending;
public:
 explicit SupervisedSeat(std::vector<std::string>command){
  int fds[2];if(pipe(fds))throw std::runtime_error("control pipe failed");control=fds[0];fcntl(control,F_SETFD,FD_CLOEXEC);fcntl(control,F_SETFL,O_NONBLOCK);
  command.insert(command.begin()+1,{"--control-fd",std::to_string(fds[1])});
  try{process=std::make_unique<Process>(command);}catch(...){close(fds[0]);close(fds[1]);throw;}
  close(fds[1]);
 }
 void ready(long long sequence,Clock::time_point deadline){
  for(;;){auto end=pending.find('\n');if(end!=std::string::npos){auto line=pending.substr(0,end);pending.erase(0,end+1);auto packet=mini_json::Json::parse(line);if(packet.at("protocol")!=mini_json::Json("sm-supervisor-control-1")||packet.at("next")!=mini_json::Json(sequence))throw std::runtime_error("invalid supervisor readiness state");return;}
   auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();if(remaining<=0)throw std::runtime_error("supervisor readiness timeout");
   pollfd fd{control,POLLIN,0};int n=poll(&fd,1,int(std::min<long long>(remaining,1000)));if(n<0&&errno==EINTR)continue;if(n<0)throw std::runtime_error("control poll failed");if(n==0)continue;
   char data[256];auto size=read(control,data,sizeof data);if(size<0&&(errno==EAGAIN||errno==EINTR))continue;if(size<=0)throw std::runtime_error("supervisor control channel closed");pending.append(data,size);if(pending.size()>1024)throw std::runtime_error("supervisor control channel overflow");
  }
 }
 Process* operator->(){return process.get();}
 Process& transport(){return *process;}
 ~SupervisedSeat(){process.reset();if(control>=0)close(control);}
};
#endif
}
