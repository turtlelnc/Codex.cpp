// Test-only HTTPS transport entry; production updater origins stay fixed.
static int portable_download_check(int argc,char** argv){try{
  if(argc!=6)return 2;auto dest=fs::absolute(fs::u8path(argv[3]));portable::Stage stage(dest.parent_path());auto cancel=std::make_shared<std::atomic<bool>>(false);std::thread cancellation;
  if(std::string(argv[5])=="cancel")cancellation=std::thread([cancel](){std::this_thread::sleep_for(std::chrono::milliseconds(100));cancel->store(true);});
  struct Join{std::thread& thread;~Join(){if(thread.joinable())thread.join();}} join{cancellation};
  auto info=plugin_object({{"url",plugin_string(argv[2])},{"size",plugin_string(argv[4])}});updater::download(info,stage.path/"download",cancel);portable::publish_file(stage.path/"download",dest);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
