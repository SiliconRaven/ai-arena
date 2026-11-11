#ifdef _WIN32
#include "include/process_windows.hpp"
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <shellapi.h>
#include <string>
int main(){
 _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
 int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return 2;
 if(argc!=3||std::wstring(argv[1])!=L"--trusted-local"){LocalFree(argv);return 2;}
 std::wstring program=argv[2];LocalFree(argv);
 if(_write(1,"#sm-native-ready\n",17)!=17)return 125;
 std::string gate;char c;while(true){if(_read(0,&c,1)!=1)return 125;if(c=='\n')break;if(gate.size()>=32)return 125;gate+=c;}
 if(gate!="#sm-step-launch")return 125;
 const wchar_t* args[]={program.c_str(),nullptr};intptr_t status=_wspawnv(_P_WAIT,program.c_str(),args);return status<0?126:int(status);
}
#else
// Trusted gated bootstrap: infrastructure readiness precedes user exec, not user initialization.
#include <unistd.h>
#include <string>
#include <cerrno>
#include <cstring>
static bool output(const char* s){size_t n=strlen(s);while(n){ssize_t k=write(1,s,n);if(k<0&&errno==EINTR)continue;if(k<=0)return false;s+=k;n-=size_t(k);}return true;}
int main(int argc,char**argv){
 const char* program="/bot/bot";
 if(argc==3&&std::string(argv[1])=="--trusted-local")program=argv[2];else if(argc!=1)return 2;
 if(!output("#sm-native-ready\n"))return 125;
 std::string gate;char c;
 // Read byte-by-byte: never prefetch bytes from the observation meant for the exec'd worker.
 while(true){ssize_t n=read(0,&c,1);if(n<0&&errno==EINTR)continue;if(n!=1)return 125;if(c=='\n')break;if(gate.size()>=32)return 125;gate+=c;}
 if(gate!="#sm-step-launch")return 125;
 char* const args[]={const_cast<char*>(program),nullptr};execv(program,args);return 126;
}

#endif
