// Trusted host-side HTTP transport. Model tools cannot invoke arbitrary host requests.
#include <curl/curl.h>
#include <cctype>
struct WebReply { long status=0; std::string body,error; std::map<std::string,std::string> headers; };
static WebReply native_http(const std::string& url,const std::string& body,
    const std::vector<std::string>& headers,bool get=false,
    const std::function<void(const char*,size_t)>& on_bytes={},
    const std::function<void()>& on_tick={},bool watch_escape=false) {
  static const bool initialized=[](){return curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;}();
  WebReply reply;if(!initialized){reply.error="HTTP initialization failed";return reply;}
  auto easy=curl_easy_init();auto multi=curl_multi_init();curl_slist* list=nullptr;
  if(!easy||!multi){if(easy)curl_easy_cleanup(easy);if(multi)curl_multi_cleanup(multi);reply.error="HTTP allocation failed";return reply;}
  struct Cleanup { CURL* easy; CURLM* multi; curl_slist*& list; ~Cleanup(){curl_multi_remove_handle(multi,easy);curl_multi_cleanup(multi);curl_easy_cleanup(easy);curl_slist_free_all(list);} } cleanup{easy,multi,list};
  for(const auto& header:headers){if(header.find_first_of("\r\n")!=std::string::npos){reply.error="Invalid HTTP header";return reply;}list=curl_slist_append(list,header.c_str());}
  curl_easy_setopt(easy,CURLOPT_URL,url.c_str());
  curl_easy_setopt(easy,CURLOPT_PROTOCOLS_STR,"http,https");
  curl_easy_setopt(easy,CURLOPT_FOLLOWLOCATION,0L);
  curl_easy_setopt(easy,CURLOPT_CONNECTTIMEOUT,20L);
  curl_easy_setopt(easy,CURLOPT_TIMEOUT,get?30L:300L);
  curl_easy_setopt(easy,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(easy,CURLOPT_HTTPHEADER,list);
  struct HeaderSink { WebReply& reply;size_t bytes=0; } header_sink{reply};
  auto header_write=+[](char* data,size_t size,size_t count,void* opaque)->size_t{
    auto& s=*static_cast<HeaderSink*>(opaque);const size_t n=size*count;
    try {
      if(n>65536-s.bytes){s.reply.error="HTTP headers exceeded 64 KB";return 0;}s.bytes+=n;
      std::string line(data,n);
      if(line.rfind("HTTP/",0)==0)s.reply.headers.clear();
      else {auto colon=line.find(':');if(colon!=std::string::npos){auto key=line.substr(0,colon);
        for(char& c:key)c=char(std::tolower(static_cast<unsigned char>(c)));
        s.reply.headers[key]=trim(line.substr(colon+1));}}
      return n;
    }catch(...){s.reply.error="HTTP header callback failed";return 0;}
  };
  curl_easy_setopt(easy,CURLOPT_HEADERFUNCTION,header_write);curl_easy_setopt(easy,CURLOPT_HEADERDATA,&header_sink);
  if(!get){curl_easy_setopt(easy,CURLOPT_POSTFIELDS,body.data());curl_easy_setopt(easy,CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(body.size()));}
  // Keep system TLS validation and the user's explicit CA/proxy configuration.
  auto ca=getenv_or("CURL_CA_BUNDLE",getenv_or("SSL_CERT_FILE"));
  auto capath=getenv_or("SSL_CERT_DIR");
#if defined(__linux__)
  if(ca.empty())for(const char* candidate:{"/etc/ssl/certs/ca-certificates.crt","/etc/pki/tls/certs/ca-bundle.crt","/etc/ssl/cert.pem"})
    if(fs::is_regular_file(candidate)){ca=candidate;break;}
#endif
  if(!ca.empty())curl_easy_setopt(easy,CURLOPT_CAINFO,ca.c_str());
  if(!capath.empty())curl_easy_setopt(easy,CURLOPT_CAPATH,capath.c_str());
  struct Sink { WebReply& reply;const std::function<void(const char*,size_t)>& callback; } sink{reply,on_bytes};
  auto write=+[](char* data,size_t size,size_t count,void* opaque)->size_t{
    auto& s=*static_cast<Sink*>(opaque);size_t n=size*count;
    try {if(n>8'000'000-s.reply.body.size()){s.reply.error="HTTP response exceeded 8 MB";return 0;}
      s.reply.body.append(data,n);if(s.callback)s.callback(data,n);return n;
    }catch(...){s.reply.error="HTTP response callback failed";return 0;}
  };
  curl_easy_setopt(easy,CURLOPT_WRITEFUNCTION,write);curl_easy_setopt(easy,CURLOPT_WRITEDATA,&sink);
  if(curl_multi_add_handle(multi,easy)!=CURLM_OK){reply.error="HTTP initialization failed";return reply;}
  TerminalInputScope input(watch_escape);auto tick_at=std::chrono::steady_clock::now();
  int running=0;CURLMcode code;
  do {
#if !defined(_WIN32)
    if(tui_interrupt_requested){reply.error="Interrupted by Ctrl+C";break;}
#endif
    if(watch_escape&&busy_input&&busy_input("",0)){reply.error="Interrupted by Esc";break;}
    code=curl_multi_perform(multi,&running);
    if(code!=CURLM_OK){reply.error="HTTP transfer failed";break;}
    const auto now=std::chrono::steady_clock::now();
    if(on_tick&&now-tick_at>=std::chrono::seconds(1)){on_tick();tick_at=now;}
    if(running&&curl_multi_poll(multi,nullptr,0,100,nullptr)!=CURLM_OK){reply.error="HTTP polling failed";break;}
  }while(running);
  int remaining=0;while(auto message=curl_multi_info_read(multi,&remaining))
    if(message->msg==CURLMSG_DONE&&message->data.result!=CURLE_OK&&reply.error.empty())
      reply.error=std::string("curl failed: ")+curl_easy_strerror(message->data.result);
  curl_easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&reply.status);return reply;
}
static WebReply web_post(const std::string& url,const std::string& body,const std::string& content_type,
    const std::vector<std::string>& headers={},bool get=false){
  auto all=headers;all.push_back("Content-Type: "+content_type);return native_http(url,body,all,get);
}
