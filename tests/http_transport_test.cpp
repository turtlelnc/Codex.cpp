#define main codex_program_main
#include "../codex.cpp"
#undef main
int main(int argc,char** argv){
  if(argc!=2)return 2;
  const std::string base=argv[1];int failed=0;
  auto check=[&](bool ok,const char* message){if(!ok){std::cerr<<"FAIL "<<message<<'\n';++failed;}};
  auto redirect=native_http(base+"/redirect","",{},true);
  check(redirect.status==302&&redirect.error.empty()&&redirect.headers["location"]=="/redirected","redirect is not followed and response headers are captured");
  auto headers=native_http(base+"/headers","",{},true);
  check(headers.error.empty()&&headers.headers["set-cookie"]=="codex_session=fixture; Secure; HttpOnly","session cookie header captured case insensitively");
  check(native_http(base+"/large-headers","",{},true).error=="HTTP headers exceeded 64 KB","bounded response headers");
  bool invalid_origin=false;try{TeamConnection::validate_origin("ftp://127.0.0.1");}catch(...){invalid_origin=true;}
  check(invalid_origin&&TeamConnection::validate_origin("https://team.example.com/")=="https://team.example.com"&&TeamConnection::validate_origin("http://127.0.0.1:8766")=="http://127.0.0.1:8766","team connections accept clean HTTP and HTTPS origins");
  auto bad=native_http(base,"",{"X-Test: x\r\nInjected: value"},true);
  check(!bad.error.empty()&&bad.status==0,"header injection rejected");
  check(!native_http("file:///etc/passwd","",{},true).error.empty(),"non HTTP protocols rejected");
  std::string streamed;int chunks=0,ticks=0;
  auto stream=native_http(base+"/stream","",{},true,[&](const char* p,size_t n){streamed.append(p,n);++chunks;},[&](){++ticks;});
  check(stream.error.empty()&&stream.status==200&&stream.body==streamed&&streamed=="data: one\n\ndata: two\n\ndata: done\n\n"&&chunks>=2&&ticks>=1,"streaming chunks and progress callbacks");
  auto callback=native_http(base+"/stream","",{},true,[](const char*,size_t){throw std::runtime_error("private callback error");});
  check(callback.error=="HTTP response callback failed","callback exception contained");
  int polls=0;busy_input=[&](const char*,size_t){return ++polls>4;};
  auto cancelled=native_http(base+"/slow","",{},true,{}, {},true);busy_input={};
  check(cancelled.error=="Interrupted by Esc","inflight request cancellation");
  auto large=native_http(base+"/large","",{},true);
  check(large.error=="HTTP response exceeded 8 MB"&&large.body.size()<=8'000'000,"bounded response memory");
  if(!failed)std::cout<<"PASS native HTTP streaming/progress/cancellation, TLS-independent protocol/header/redirect guards and response bound\n";
  return failed?1:0;
}
