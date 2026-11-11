#pragma once
// Cooperative protocol adapter only. Deadlines and authoritative memory are owned by the external supervisor.
#include "json.hpp"
#include <iostream>
#include <sstream>
#include <utility>
namespace sm_step_worker {
using J=mini_json::Json;
inline std::string ascii(const std::string&s,size_t limit){std::string out;for(unsigned char c:s){if(out.size()+4>limit)break;if(c<128)out+=char(c);else{const char*h="0123456789abcdef";out+="\\x";out+=h[c>>4];out+=h[c&15];}}return out;}
struct Capture:std::streambuf{std::string kept;size_t dropped=0;int overflow(int c)override{if(c==traits_type::eof())return traits_type::not_eof(c);if(kept.size()<2000)kept+=char(c);else ++dropped;return c;}std::streamsize xsputn(const char*s,std::streamsize n)override{for(std::streamsize i=0;i<n;++i)overflow(static_cast<unsigned char>(s[i]));return n;}};
struct Redirect{Capture capture;std::istringstream empty;std::streambuf *out,*err,*in;Redirect():out(std::cout.rdbuf(&capture)),err(std::cerr.rdbuf(&capture)),in(std::cin.rdbuf(empty.rdbuf())){}~Redirect(){std::cout.rdbuf(out);std::cerr.rdbuf(err);std::cin.rdbuf(in);}};
inline bool line(std::string&out){out.clear();char c;while(std::cin.get(c)){if(c=='\n')return true;if(out.size()>=2*1024*1024)throw std::runtime_error("step request exceeds bound");out+=c;}return !out.empty();}
template<class Step>int run(Step step){
 std::string text;
 try{while(line(text)){
  J request=J::parse(text);if(request.at("protocol")!=J("sm-step-json-1"))return 2;
  J order=nullptr,memory=nullptr,error=nullptr;std::string debug;
  {Redirect redirected;try{J input_memory=request.at("memory");auto result=step(request.at("observation"),input_memory);if(!result.first.is_null()&&!result.first.is_object())throw std::runtime_error("order must be an object or null");if(result.second.dump().size()>32768)throw std::runtime_error("JSON memory exceeds 32768 bytes");order=result.first;memory=result.second;}catch(const std::exception&e){error=ascii(e.what(),300);}catch(...){error="unknown native step exception";}debug=ascii(redirected.capture.kept,2000);}
  J response{{"protocol","sm-step-json-1"},{"sequence",request.at("sequence")},{"order",order},{"memory",memory},{"error",error}};
  if(!debug.empty())response["debug"]=debug;
  auto encoded=response.dump();if(encoded.size()+1>65536){response.erase("debug");encoded=response.dump();}
  if(encoded.size()+1>65536){response["order"]=nullptr;response["memory"]=nullptr;response["error"]="native order exceeds transport bound";encoded=response.dump();}
  std::cout<<encoded<<std::endl;
 }}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}return 0;
}
template<class Policy>int run_policy(Policy policy){return run([&](const J&obs,J&memory){return std::make_pair(policy(obs),memory);});}
}
