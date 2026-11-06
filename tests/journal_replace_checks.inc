static int journal_replace_checks(const fs::path& root){
  try{
    fs::create_directories(root);Session session;session.dir=root;session.items_path=root/"replace.items.jsonl";
    const std::string old=R"({"role":"user","content":"original history"})",next=R"({"role":"user","content":"compacted history"})";
    {std::ofstream file(session.items_path);file<<old<<'\n';}
    session.items={old};
#if defined(_WIN32)
    auto hold=[&](){return CreateFileW(session.items_path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);};
    auto reader=hold();if(reader==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create journal reader fixture");
    std::thread release([reader](){std::this_thread::sleep_for(std::chrono::milliseconds(100));CloseHandle(reader);});
    const bool changed=session.replace_items({next});release.join();if(!changed)throw std::runtime_error("Transient journal reader blocked atomic replacement: "+session.storage_error);
    auto persistent=hold();if(persistent==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create persistent journal reader fixture");
    const auto before=std::chrono::steady_clock::now();const bool refused=!session.replace_items({old});CloseHandle(persistent);
    if(!refused||std::chrono::steady_clock::now()-before>std::chrono::seconds(5)||session.items!=std::vector<std::string>{next}||slurp(session.items_path)!=next+"\n")throw std::runtime_error("Locked journal did not preserve original disk/memory state within deadline");
    for(const auto& file:fs::directory_iterator(root))if(file.path().filename().string().find("journal-new")!=std::string::npos)throw std::runtime_error("Failed journal replacement left a staging file");
#else
    if(!session.replace_items({next})||slurp(session.items_path)!=next+"\n")throw std::runtime_error("Atomic journal replacement failed");
#endif
    std::cout<<"PASS atomic journal replacement; Windows transient/permanent reader locks preserve history and bound retries\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
