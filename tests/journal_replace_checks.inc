#if !defined(_WIN32)
#include <sys/resource.h>
#endif
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
    Session image_session;
    std::string error;
    if (!init_session(image_session, root, false, std::nullopt, error))
      throw std::runtime_error(error);
    const auto id = image_session.id;
    const std::string image = "data:image/png;base64," + base64_encode(std::string(5'000'000, 'x'));
    const auto attachment = plugin_object({{"type", plugin_string("input_image")}, {"image_url", plugin_string(image)}});
    const auto item = json_dump(plugin_object({{"role", plugin_string("user")}, {"content", native_web::arr({attachment, attachment})}}));
    if (!image_session.record_item(item)) throw std::runtime_error("Cannot append two-image journal record");
    image_session.release_lock();
    Session restored;
    if (!init_session(restored, root, false, id, error) || restored.items != std::vector<std::string>{item} ||
        !restored.record_item(next))
      throw std::runtime_error("Cannot resume and append after two-image record: " + error);

    const auto state = root / "large-state.json";
    const auto large_state = plugin_object({{"content", plugin_string(std::string(17 * 1024 * 1024, 'x'))}});
    native_web::atomic_save(state, large_state, native_web::HostedService::database_limit);
    bool oversized_refused = false;
    try { native_web::atomic_save(state, large_state, 16 * 1024 * 1024); }
    catch (const std::runtime_error&) { oversized_refused = true; }
    if (!oversized_refused || native_web::text(native_web::read_json(state, native_web::HostedService::database_limit), "content").size() != 17 * 1024 * 1024)
      throw std::runtime_error("State size check did not preserve the previous durable state");

#if !defined(_WIN32)
    native_web::Workspace workspace(root);
    const auto filename = root / "editor.txt";
    { std::ofstream file(filename); file << "original"; }
    struct rlimit original_limit{};
    if (::getrlimit(RLIMIT_FSIZE, &original_limit) != 0)
      throw std::runtime_error("Cannot inspect file size limit");
    struct rlimit limited = original_limit;
    limited.rlim_cur = 32;
    if (::setrlimit(RLIMIT_FSIZE, &limited) != 0)
      throw std::runtime_error("Cannot set file size limit");
    const auto prior_signal = ::signal(SIGXFSZ, SIG_IGN);
    bool write_failed = false;
    try { workspace.write("editor.txt", std::string(64, 'x'), native_web::digest("original")); }
    catch (const std::runtime_error&) { write_failed = true; }
    ::signal(SIGXFSZ, prior_signal);
    if (::setrlimit(RLIMIT_FSIZE, &original_limit) != 0 || !write_failed || slurp(filename) != "original")
      throw std::runtime_error("Partial editor save changed the original file");
    for (const auto& file : fs::directory_iterator(root))
      if (file.path().filename().u8string().rfind(".codex-editor-", 0) == 0)
        throw std::runtime_error("Editor save left a staging file");
    workspace.write("editor.txt", "updated", native_web::digest("original"));
    if (slurp(filename) != "updated") throw std::runtime_error("Editor retry did not save");
#endif

    std::cout<<"PASS journal replacement, multi-image resume, state size checks and POSIX editor write recovery\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
