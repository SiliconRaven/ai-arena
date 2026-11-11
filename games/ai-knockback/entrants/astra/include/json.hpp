#pragma once
// Standalone C++17 JSON. Integers preserve arbitrary precision decimal tokens;
// as_int() checks signed 64-bit range. Non-finite floats are errors. Strict UTF-8.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
namespace mini_json {
class Json {
public:
 using array=std::vector<Json>; using object=std::map<std::string,Json>;
private:
 struct BigInt {std::string decimal;bool operator==(const BigInt&o)const{return decimal==o.decimal;}};
 std::variant<std::nullptr_t,bool,int64_t,double,std::string,array,object,BigInt> v_;
 static uint32_t cp(const std::string&s,size_t&i) {
  if(i>=s.size()) throw std::runtime_error("truncated UTF-8");
  auto b=static_cast<unsigned char>(s[i++]); if(b<128)return b;
  unsigned n; uint32_t c, min;
  if(b>=0xc2&&b<=0xdf){n=1;c=b&31;min=0x80;}
  else if(b>=0xe0&&b<=0xef){n=2;c=b&15;min=0x800;}
  else if(b>=0xf0&&b<=0xf4){n=3;c=b&7;min=0x10000;}
  else throw std::runtime_error("invalid UTF-8 leading byte");
  while(n--){if(i>=s.size())throw std::runtime_error("truncated UTF-8");auto x=static_cast<unsigned char>(s[i++]);if((x&0xc0)!=0x80)throw std::runtime_error("invalid UTF-8 continuation");c=(c<<6)|(x&63);}
  if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))throw std::runtime_error("invalid UTF-8 code point");
  return c;
 }
 static void utf8(std::string&s,uint32_t c){if(c<128)s+=char(c);else if(c<0x800){s+=char(0xc0|(c>>6));s+=char(0x80|(c&63));}else if(c<0x10000){s+=char(0xe0|(c>>12));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}else{s+=char(0xf0|(c>>18));s+=char(0x80|((c>>12)&63));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}}
 static void hex(std::string&o,uint32_t c){static const char*h="0123456789abcdef";o+="\\u";for(int k=12;k>=0;k-=4)o+=h[(c>>k)&15];}
 static void quote(std::string&o,const std::string&s,bool ascii){o+='"';for(size_t i=0;i<s.size();){size_t start=i;uint32_t c=cp(s,i);switch(c){case '"':o+="\\\"";break;case '\\':o+="\\\\";break;case '\b':o+="\\b";break;case '\f':o+="\\f";break;case '\n':o+="\\n";break;case '\r':o+="\\r";break;case '\t':o+="\\t";break;default:if(c<32||(ascii&&c>=128)){if(c<=0xffff)hex(o,c);else{c-=0x10000;hex(o,0xd800+(c>>10));hex(o,0xdc00+(c&1023));}}else o.append(s,start,i-start);}}o+='"';}
 void emit(std::string&o,bool ascii)const{if(is_null())o+="null";else if(is_bool())o+=as_bool()?"true":"false";else if(is_int())o+=integer_decimal();else if(std::holds_alternative<double>(v_)){double d=as_double();if(!std::isfinite(d))throw std::runtime_error("nonfinite JSON number");char buf[128];auto r=std::to_chars(buf,buf+sizeof buf,d,std::chars_format::general);if(r.ec!=std::errc())throw std::runtime_error("float serialization failed");std::string s(buf,r.ptr);if(s.find_first_of(".eE")==std::string::npos)s+=".0";o+=s;}else if(is_string())quote(o,as_string(),ascii);else if(is_array()){o+='[';bool first=true;for(auto&x:as_array()){if(!first)o+=',';first=false;x.emit(o,ascii);}o+=']';}else{o+='{';bool first=true;for(auto&x:as_object()){if(!first)o+=',';first=false;quote(o,x.first,ascii);o+=':';x.second.emit(o,ascii);}o+='}';}}
 struct Parser {
  const std::string&s;size_t i=0; size_t max_depth;
  [[noreturn]] void fail(const char*m)const{throw std::runtime_error(std::string("JSON at byte ")+std::to_string(i)+": "+m);}
  void ws(){while(i<s.size()&&(s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'))++i;}
  bool eat(char c){if(i<s.size()&&s[i]==c){++i;return true;}return false;}
  uint32_t h4(){uint32_t n=0;for(int k=0;k<4;++k){if(i>=s.size())fail("truncated unicode escape");char c=s[i++];n<<=4;if(c>='0'&&c<='9')n+=c-'0';else if(c>='a'&&c<='f')n+=c-'a'+10;else if(c>='A'&&c<='F')n+=c-'A'+10;else fail("bad unicode escape");}return n;}
  std::string str(){if(!eat('"'))fail("expected string");std::string out;while(i<s.size()){unsigned char c=s[i++];if(c=='"')return out;if(c<32)fail("unescaped control character");if(c=='\\'){if(i==s.size())fail("truncated escape");char e=s[i++];switch(e){case '"':case '\\':case '/':out+=e;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;case 'u':{uint32_t u=h4();if(u>=0xd800&&u<=0xdbff){if(!eat('\\')||!eat('u'))fail("missing low surrogate");uint32_t lo=h4();if(lo<0xdc00||lo>0xdfff)fail("invalid low surrogate");u=0x10000+((u-0xd800)<<10)+(lo-0xdc00);}else if(u>=0xdc00&&u<=0xdfff)fail("unpaired low surrogate");utf8(out,u);break;}default:fail("invalid escape");}}else if(c<128)out+=char(c);else{size_t start=--i;cp(s,i);out.append(s,start,i-start);}}fail("unterminated string");}
  Json val(size_t depth){if(depth>max_depth)fail("nesting limit exceeded");ws();if(i==s.size())fail("expected value");char c=s[i];if(c=='"')return Json(str());if(eat('{')){object o;ws();if(eat('}'))return o;while(true){ws();std::string k=str();ws();if(!eat(':'))fail("expected colon");Json x=val(depth+1);if(!o.emplace(k,std::move(x)).second)fail("duplicate object key");ws();if(eat('}'))return o;if(!eat(','))fail("expected comma");}}if(eat('[')){array a;ws();if(eat(']'))return a;while(true){a.push_back(val(depth+1));ws();if(eat(']'))return a;if(!eat(','))fail("expected comma");}}
  for(auto lit:{"true","false","null"}){size_t n=std::char_traits<char>::length(lit);if(s.compare(i,n,lit)==0){i+=n;if(lit[0]=='n')return nullptr;return lit[0]=='t';}}
  size_t start=i;eat('-');if(eat('0')){if(i<s.size()&&s[i]>='0'&&s[i]<='9')fail("leading zero");}else{if(i>=s.size()||s[i]<'1'||s[i]>'9')fail("invalid number");while(i<s.size()&&s[i]>='0'&&s[i]<='9')++i;}bool fp=false;if(eat('.')){fp=true;size_t p=i;while(i<s.size()&&s[i]>='0'&&s[i]<='9')++i;if(p==i)fail("missing fraction");}if(i<s.size()&&(s[i]=='e'||s[i]=='E')){fp=true;++i;if(i<s.size()&&(s[i]=='+'||s[i]=='-'))++i;size_t p=i;while(i<s.size()&&s[i]>='0'&&s[i]<='9')++i;if(p==i)fail("missing exponent");}if(!fp){int64_t n;auto r=std::from_chars(s.data()+start,s.data()+i,n);if(r.ec==std::errc::result_out_of_range){Json out;out.v_=BigInt{s.substr(start,i-start)};return out;}if(r.ec!=std::errc())fail("invalid integer");return n;}double d;auto r=std::from_chars(s.data()+start,s.data()+i,d,std::chars_format::general);if(r.ec!=std::errc()||!std::isfinite(d))fail("floating number out of range");return d;
  }
 };
public:
 Json():v_(nullptr){} Json(std::nullptr_t):v_(nullptr){} Json(bool b):v_(b){}
 template<class T, std::enable_if_t<std::is_integral_v<T>&&!std::is_same_v<T,bool>,int> =0> Json(T n){if constexpr(std::is_unsigned_v<T>){if(n>static_cast<uint64_t>(INT64_MAX))throw std::runtime_error("unsigned integer out of int64 range");}v_=static_cast<int64_t>(n);}
 Json(std::initializer_list<object::value_type> entries):v_(object{}){for(const auto& e:entries)if(!as_object().emplace(e).second)throw std::runtime_error("duplicate object key");}
 bool empty()const{return size()==0;}
 Json(double d):v_(d){if(!std::isfinite(d))throw std::runtime_error("nonfinite JSON number");} Json(const char*s):v_(std::string(s)){} Json(std::string s):v_(std::move(s)){} Json(array a):v_(std::move(a)){} Json(object o):v_(std::move(o)){}
 static Json parse(const std::string&s,size_t max_depth=256){Parser p{s,0,max_depth};Json j=p.val(0);p.ws();if(p.i!=s.size())p.fail("trailing data");return j;}
 std::string integer_decimal()const{if(std::holds_alternative<BigInt>(v_))return std::get<BigInt>(v_).decimal;return std::to_string(as_int());}
 static Json from_integer_decimal(const std::string&s){auto j=parse(s);if(!j.is_int())throw std::runtime_error("expected integer decimal");return j;}
 std::string dump(bool ensure_ascii=true)const{std::string o;emit(o,ensure_ascii);return o;}
 bool is_null()const{return std::holds_alternative<std::nullptr_t>(v_);}bool is_bool()const{return std::holds_alternative<bool>(v_);}bool is_int()const{return std::holds_alternative<int64_t>(v_)||std::holds_alternative<BigInt>(v_);}bool is_number()const{return is_int()||std::holds_alternative<double>(v_);}bool is_string()const{return std::holds_alternative<std::string>(v_);}bool is_array()const{return std::holds_alternative<array>(v_);}bool is_object()const{return std::holds_alternative<object>(v_);}bool is_boolean()const{return is_bool();}bool is_number_integer()const{return is_int();}
 bool as_bool()const{return std::get<bool>(v_);}int64_t as_int()const{if(std::holds_alternative<BigInt>(v_))throw std::out_of_range("JSON integer does not fit int64");return std::get<int64_t>(v_);}double as_double()const{return is_int()?double(as_int()):std::get<double>(v_);}const std::string&as_string()const{return std::get<std::string>(v_);}const array&as_array()const{return std::get<array>(v_);}array&as_array(){return std::get<array>(v_);}const object&as_object()const{return std::get<object>(v_);}object&as_object(){return std::get<object>(v_);}
 bool contains(const std::string&k)const{return is_object()&&as_object().count(k)!=0;}const Json&at(const std::string&k)const{return as_object().at(k);}Json&at(const std::string&k){return as_object().at(k);}const Json&at(size_t n)const{return as_array().at(n);}Json&at(size_t n){return as_array().at(n);}Json&operator[](const std::string&k){if(is_null())v_=object{};return as_object()[k];}const Json&operator[](const std::string&k)const{return at(k);}Json&operator[](size_t n){return at(n);}const Json&operator[](size_t n)const{return at(n);}size_t size()const{if(is_array())return as_array().size();if(is_object())return as_object().size();if(is_string())return as_string().size();return is_null()?0:1;}void push_back(Json x){if(is_null())v_=array{};as_array().push_back(std::move(x));}
 template<class T>T get()const{if constexpr(std::is_same_v<T,Json>)return *this;else if constexpr(std::is_same_v<T,std::string>)return as_string();else if constexpr(std::is_same_v<T,bool>)return as_bool();else if constexpr(std::is_integral_v<T>){auto n=as_int();if constexpr(std::is_unsigned_v<T>){if(n<0||static_cast<uint64_t>(n)>std::numeric_limits<T>::max())throw std::runtime_error("integer conversion overflow");}else{if(n<std::numeric_limits<T>::min()||n>std::numeric_limits<T>::max())throw std::runtime_error("integer conversion overflow");}return static_cast<T>(n);}else if constexpr(std::is_floating_point_v<T>)return static_cast<T>(as_double());else if constexpr(std::is_same_v<T,array>)return as_array();else if constexpr(std::is_same_v<T,object>)return as_object();else {T out;for(const auto&x:as_array())out.push_back(x.template get<typename T::value_type>());return out;}}
 template<class T>T value(const std::string&k,T fallback)const{return contains(k)?at(k).template get<T>():fallback;}std::string value(const std::string&k,const char*fallback)const{return contains(k)?at(k).as_string():std::string(fallback);}
 size_t erase(const std::string&k){return as_object().erase(k);}
 bool operator==(const Json&o)const{return v_==o.v_;}bool operator!=(const Json&o)const{return !(*this==o);}
};
using json=Json;
}
