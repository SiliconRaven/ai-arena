#pragma once
// POSIX subprocess transport for trusted local bots; not a security sandbox.
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

namespace knockback {
using Clock=std::chrono::steady_clock;
struct Answer {Clock::time_point arrived; std::string text; bool too_long=false; bool eof=false;};
class Process {
    pid_t pid=-1; int in=-1,out=-1,err=-1;
    std::mutex read_mutex,write_mutex; std::condition_variable ready,writable;
    std::deque<Answer> inbox; std::deque<std::string> outbox;
    bool ended=false,stopping=false; Clock::time_point ended_at;
    std::thread reader,writer,logger;
    static void pipe_cloexec(int (&fds)[2]) {
        if(pipe(fds)<0) throw std::runtime_error("pipe failed");
        for(int f:fds) if(fcntl(f,F_SETFD,FD_CLOEXEC)<0) {close(fds[0]);close(fds[1]);throw std::runtime_error("fcntl failed");}
    }
    void arrive(Answer a) {
        std::lock_guard<std::mutex> lock(read_mutex);
        if(inbox.size()>=64) {inbox.pop_front();++dropped_lines;}
        inbox.push_back(std::move(a));ready.notify_all();
    }
    void read_loop() {
        char bytes[65536];std::string line;bool skip=false;ssize_t count;
        while((count=read(out,bytes,sizeof bytes))!=0) {
            if(count<0){if(errno==EINTR)continue;break;}
            auto now=Clock::now();
            for(ssize_t i=0;i<count;++i){char c=bytes[i];if(c=='\n'){if(!skip)arrive({now,line});line.clear();skip=false;}else if(!skip){line+=c;if(line.size()>65536){line.clear();skip=true;++long_lines;arrive({now,"",true});}}}
        }
        if(!line.empty())arrive({Clock::now(),line});
        std::lock_guard<std::mutex> lock(read_mutex);ended=true;ended_at=Clock::now();ready.notify_all();
    }
    void write_loop() {
        for(;;){std::string text;{std::unique_lock<std::mutex> lock(write_mutex);writable.wait(lock,[&]{return stopping||!outbox.empty();});if(stopping)break;text=std::move(outbox.front());outbox.pop_front();}
            size_t offset=0;while(offset<text.size()){ssize_t n=write(in,text.data()+offset,text.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0){broken=true;return;}offset+=size_t(n);}
        }
    }
    void log_loop(){char buf[65536];ssize_t n;while((n=read(err,buf,sizeof buf))!=0){if(n<0){if(errno==EINTR)continue;break;}stderr_bytes+=size_t(n);}}
public:
    std::atomic<size_t> dropped_lines{0},long_lines{0},unsent{0},stderr_bytes{0};std::atomic<bool> broken{false};
    explicit Process(const std::vector<std::string>&command) {
        if(command.empty())throw std::runtime_error("empty bot command");
        std::vector<char*> argv;for(auto&s:command)argv.push_back(const_cast<char*>(s.c_str()));argv.push_back(nullptr);
        int a[2]={-1,-1},b[2]={-1,-1},c[2]={-1,-1};
        try{pipe_cloexec(a);pipe_cloexec(b);pipe_cloexec(c);}catch(...){for(int f:{a[0],a[1],b[0],b[1],c[0],c[1]})if(f>=0)close(f);throw;}
        pid=fork();
        if(pid==0){setpgid(0,0);dup2(a[0],STDIN_FILENO);dup2(b[1],STDOUT_FILENO);dup2(c[1],STDERR_FILENO);for(int f:{a[0],a[1],b[0],b[1],c[0],c[1]})close(f);execvp(argv[0],argv.data());_exit(127);}
        if(pid<0){for(int f:{a[0],a[1],b[0],b[1],c[0],c[1]})close(f);throw std::runtime_error("fork failed");}
        setpgid(pid,pid);close(a[0]);close(b[1]);close(c[1]);in=a[1];out=b[0];err=c[0];
        reader=std::thread([this]{read_loop();});writer=std::thread([this]{write_loop();});logger=std::thread([this]{log_loop();});
    }
    Process(const Process&)=delete;Process&operator=(const Process&)=delete;
    bool send(std::string text){if(broken)return false;std::lock_guard<std::mutex> lock(write_mutex);if(outbox.size()>=2){++unsent;return false;}outbox.push_back(std::move(text));writable.notify_one();return true;}
    std::optional<Answer> receive(Clock::time_point deadline){std::unique_lock<std::mutex> lock(read_mutex);ready.wait_until(lock,deadline,[&]{return ended||!inbox.empty();});if(!inbox.empty()){auto a=std::move(inbox.front());inbox.pop_front();return a;}if(ended)return Answer{ended_at,"",false,true};return std::nullopt;}
    bool exited(){std::lock_guard<std::mutex> lock(read_mutex);return ended;}
    ~Process(){
        {std::lock_guard<std::mutex> lock(write_mutex);stopping=true;outbox.clear();writable.notify_all();}
        if(pid>0){kill(-pid,SIGTERM);std::this_thread::sleep_for(std::chrono::milliseconds(20));kill(-pid,SIGKILL);int status;while(waitpid(pid,&status,0)<0&&errno==EINTR){}}
        if(writer.joinable())writer.join();if(reader.joinable())reader.join();if(logger.joinable())logger.join();
        for(int fd:{in,out,err})if(fd>=0)close(fd);
    }
};
}
#endif
