#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>
#include "sha2.hpp"
// CPython random.Random integer-seed MT19937 and _randbelow/getrandbits path.
namespace mini_json {
class PythonRandom {
 std::array<uint32_t,624>mt{};size_t index=624;
 uint32_t next(){if(index>=624){for(size_t i=0;i<624;++i){uint32_t y=(mt[i]&0x80000000U)|(mt[(i+1)%624]&0x7fffffffU);mt[i]=mt[(i+397)%624]^(y>>1)^((y&1)?0x9908b0dfU:0);}index=0;}uint32_t y=mt[index++];y^=y>>11;y^=(y<<7)&0x9d2c5680U;y^=(y<<15)&0xefc60000U;y^=y>>18;return y;}
 static std::vector<uint32_t> words(std::string s,uint32_t mask){bool neg=!s.empty()&&s[0]=='-';if(neg)s.erase(s.begin());std::vector<uint32_t>a(1,0);for(char c:s){if(c<'0'||c>'9')throw std::runtime_error("invalid random seed");uint64_t carry=c-'0';for(auto&x:a){uint64_t z=uint64_t(x)*10+carry;x=uint32_t(z);carry=z>>32;}if(carry)a.push_back(uint32_t(carry));}if(mask){if(neg){size_t i=0;while(a[i]==0){a[i++]=UINT32_MAX;}--a[i];}a[0]^=mask;if(neg){uint64_t carry=1;for(auto&x:a){uint64_t z=uint64_t(x)+carry;x=uint32_t(z);carry=z>>32;}if(carry)a.push_back(uint32_t(carry));}}while(a.size()>1&&!a.back())a.pop_back();return a;}
public:
 explicit PythonRandom(std::string seed,uint32_t xor_mask=0):PythonRandom(words(seed,xor_mask)){}
 explicit PythonRandom(const std::vector<uint32_t>&key){mt[0]=19650218U;for(size_t i=1;i<624;++i)mt[i]=1812433253U*(mt[i-1]^(mt[i-1]>>30))+uint32_t(i);size_t i=1,j=0;for(size_t k=std::max(size_t(624),key.size());k;--k){mt[i]=(mt[i]^((mt[i-1]^(mt[i-1]>>30))*1664525U))+key[j]+uint32_t(j);if(++i>=624){mt[0]=mt[623];i=1;}if(++j>=key.size())j=0;}for(size_t k=623;k;--k){mt[i]=(mt[i]^((mt[i-1]^(mt[i-1]>>30))*1566083941U))-uint32_t(i);if(++i>=624){mt[0]=mt[623];i=1;}}mt[0]=0x80000000U;}
 static PythonRandom from_string(const std::string& seed){
  std::string bytes=seed+sha2::sha512_bytes(seed);
  std::vector<uint32_t> key; for(size_t end=bytes.size();end;){uint32_t word=0; unsigned shift=0;for(int i=0;i<4&&end;++i){word|=uint32_t(static_cast<unsigned char>(bytes[--end]))<<shift;shift+=8;}key.push_back(word);}return PythonRandom(key);
 }
 double random(){uint32_t a=next()>>5,b=next()>>6;return (a*67108864.0+b)/9007199254740992.0;}
 int randint(int a,int b){return a+int(randbelow(uint32_t(b-a+1)));}
 template<class T> std::vector<T> sample(const std::vector<T>& values,size_t k){
  size_t n=values.size();if(k>n)throw std::runtime_error("sample larger than population");std::vector<T> result;size_t setsize=21;if(k>5){size_t p=1;while(p<3*k)p*=4;setsize+=p;}
  if(n<=setsize){auto pool=values;for(size_t i=0;i<k;++i){size_t j=randbelow(n-i);result.push_back(pool[j]);pool[j]=pool[n-i-1];}}
  else{std::set<size_t> selected;for(size_t i=0;i<k;++i){size_t j=randbelow(n);while(selected.count(j))j=randbelow(n);selected.insert(j);result.push_back(values[j]);}}return result;
 }
 uint32_t randbelow(uint32_t n){if(!n)throw std::runtime_error("empty random choice");unsigned k=0;for(uint32_t x=n;x;x>>=1)++k;uint32_t r;do{r=next()>>(32-k);}while(r>=n);return r;}
 template<class T>void shuffle(std::vector<T>&a){for(size_t i=a.size();i>1;--i)std::swap(a[i-1],a[randbelow(uint32_t(i))]);}
 int choice(std::initializer_list<int>v){return *(v.begin()+randbelow(uint32_t(v.size())));}
};
}
