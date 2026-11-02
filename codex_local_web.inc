// User slash command owns a loopback service, never an Agent-exposed launch tool.
struct LocalWebBridge{
  std::unique_ptr<native_web::Child> service;std::string url;bool paused=false;
  bool running(){return service&&!url.empty()&&service->running();}
  ~LocalWebBridge(){stop();}
  void stop(){if(service){
#if !defined(_WIN32)
    if(service->pid>0&&!service->ended){::kill(service->pid,SIGTERM);auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(service->running()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(20));}
#endif
    service.reset();}url.clear();}
  std::string start(const Options& options,bool open){
    if(!running()){
#if defined(_WIN32)
      WSADATA data;native_web::require(WSAStartup(MAKEWORD(2,2),&data)==0,"Cannot initialize sockets");SOCKET socket_fd=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);native_web::require(socket_fd!=INVALID_SOCKET,"Cannot reserve local port");
#else
      int socket_fd=::socket(AF_INET,SOCK_STREAM,0);native_web::require(socket_fd>=0,"Cannot reserve local port");
#endif
      sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;int bound=::bind(socket_fd,reinterpret_cast<sockaddr*>(&address),sizeof address);
#if defined(_WIN32)
      int size=sizeof address;int named=getsockname(socket_fd,reinterpret_cast<sockaddr*>(&address),&size);closesocket(socket_fd);WSACleanup();
#else
      socklen_t size=sizeof address;int named=getsockname(socket_fd,reinterpret_cast<sockaddr*>(&address),&size);::close(socket_fd);
#endif
      native_web::require(bound==0&&named==0,"Cannot select local web port");const auto port=std::to_string(ntohs(address.sin_port));url="http://127.0.0.1:"+port;
      std::vector<std::string> args={updater::self.u8string()};for(const auto& path:plugin_manifests){auto frozen=plugin_worker_manifests.find(path);native_web::require(frozen!=plugin_worker_manifests.end(),"Native plugin snapshot unavailable");args.insert(args.end(),{"--plugin-reviewed",frozen->second,plugin_reviews.at(path)});}args.insert(args.end(),{"web","--root",options.root.u8string(),"--port",port,"--provider",options.provider,"--model",options.model});if(!options.base_url.empty())args.insert(args.end(),{"--base-url",options.base_url,"--api",options.api_style==ApiStyle::ChatCompletions?"chat":"responses"});if(options.no_api_key)args.push_back("--no-api-key");else if(!options.api_key_env.empty())args.insert(args.end(),{"--api-key-env",options.api_key_env});if(!options.reasoning_effort.empty())args.insert(args.end(),{"--reasoning",options.reasoning_effort});service=std::make_unique<native_web::Child>();service->start(args);std::string output;auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);bool ready=false;
      while(std::chrono::steady_clock::now()<end){output+=service->receive();if(output.find("web: "+url)!=std::string::npos){ready=true;break;}if(!service->running())break;std::this_thread::sleep_for(std::chrono::milliseconds(20));}if(!ready){stop();throw std::runtime_error("Web service did not start: "+output);}paused=true;
    }
    std::string message="WebUI: "+url+"\nSame workspace and saved sessions; terminal task execution is paused. Use /web stop to return. This is a separate Agent process, not a live handoff.";
    if(open)try{
#if defined(_WIN32)
      auto wide=fs::u8path(url).wstring();native_web::require(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",wide.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32,"Browser could not be opened");
#elif defined(__APPLE__)
      host_detached({"/usr/bin/open",url});
#else
      host_detached({"/usr/bin/xdg-open",url});
#endif
    }catch(const std::exception& e){message+="\nBrowser launch unavailable; open the URL manually: "+std::string(e.what());}
    return message;
  }
};
