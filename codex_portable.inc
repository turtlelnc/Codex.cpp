// Host-only data transfer. Never registered as an Agent tool or WebUI RPC.
#if !defined(_WIN32)
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif
namespace portable {
using native_web::require;
using Files=std::map<std::string,std::string>;
using Modes=std::set<std::string>;
constexpr size_t limit=128*1024*1024;
static void wipe(std::string& s){volatile char* p=s.empty()?nullptr:&s[0];for(size_t i=0;i<s.size();++i)p[i]=0;}
static std::string random_bytes(size_t n){std::string s(n,'\0');
#if defined(_WIN32)
  require(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&s[0]),static_cast<ULONG>(n),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"Secure RNG failed");
#else
  require(RAND_bytes(reinterpret_cast<unsigned char*>(&s[0]),static_cast<int>(n))==1,"Secure RNG failed");
#endif
  return s;
}
// Fixed work factor and authenticated format; untrusted input cannot request
// arbitrary KDF cost. No passwords are accepted as literal command arguments.
static std::string crypt(bool encrypt,const std::string& input,const std::string& password,const std::string& salt,const std::string& nonce,std::string& tag){
  require(!password.empty()&&password.size()<=4096&&salt.size()==16&&nonce.size()==12&&tag.size()==16,"Invalid encrypted pack parameters");
  std::string key(32,'\0');struct Clear{std::string& value;~Clear(){wipe(value);}} clear{key};std::string output(input.size()+16,'\0');
  const std::string aad="CODEXPACK1:E:PBKDF2-SHA256:600000:AES-256-GCM";
#if defined(_WIN32)
  struct Algorithm{BCRYPT_ALG_HANDLE h=nullptr;~Algorithm(){if(h)BCryptCloseAlgorithmProvider(h,0);}} hash,aes;
  require(BCryptOpenAlgorithmProvider(&hash.h,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG)==0,"KDF provider failed");
  require(BCryptDeriveKeyPBKDF2(hash.h,reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),static_cast<ULONG>(password.size()),reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())),16,600000,reinterpret_cast<PUCHAR>(&key[0]),32,0)==0,"KDF failed");
  require(BCryptOpenAlgorithmProvider(&aes.h,BCRYPT_AES_ALGORITHM,nullptr,0)==0&&BCryptSetProperty(aes.h,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),sizeof(BCRYPT_CHAIN_MODE_GCM),0)==0,"AES-GCM provider failed");
  struct Key{BCRYPT_KEY_HANDLE h=nullptr;~Key(){if(h)BCryptDestroyKey(h);}} symmetric;
  require(BCryptGenerateSymmetricKey(aes.h,&symmetric.h,nullptr,0,reinterpret_cast<PUCHAR>(&key[0]),32,0)==0,"AES key failed");
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);info.pbNonce=reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.data()));info.cbNonce=12;info.pbTag=reinterpret_cast<PUCHAR>(&tag[0]);info.cbTag=16;info.pbAuthData=reinterpret_cast<PUCHAR>(const_cast<char*>(aad.data()));info.cbAuthData=static_cast<ULONG>(aad.size());ULONG written=0;
  auto operation=encrypt?BCryptEncrypt:BCryptDecrypt;auto status=operation(symmetric.h,reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),static_cast<ULONG>(input.size()),&info,nullptr,0,reinterpret_cast<PUCHAR>(&output[0]),static_cast<ULONG>(output.size()),&written,0);
  if(status!=0){wipe(output);throw std::runtime_error("Wrong password or damaged encrypted pack");}output.resize(written);
#else
  require(PKCS5_PBKDF2_HMAC(password.data(),static_cast<int>(password.size()),reinterpret_cast<const unsigned char*>(salt.data()),16,600000,EVP_sha256(),32,reinterpret_cast<unsigned char*>(&key[0]))==1,"KDF failed");
  std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);require(bool(ctx),"AES context failed");int n=0,written=0;
  require(EVP_CipherInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,nullptr,nullptr,encrypt?1:0)==1&&EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_IVLEN,12,nullptr)==1&&EVP_CipherInit_ex(ctx.get(),nullptr,nullptr,reinterpret_cast<const unsigned char*>(key.data()),reinterpret_cast<const unsigned char*>(nonce.data()),encrypt?1:0)==1,"AES init failed");
  require(EVP_CipherUpdate(ctx.get(),nullptr,&n,reinterpret_cast<const unsigned char*>(aad.data()),static_cast<int>(aad.size()))==1&&EVP_CipherUpdate(ctx.get(),reinterpret_cast<unsigned char*>(&output[0]),&written,reinterpret_cast<const unsigned char*>(input.data()),static_cast<int>(input.size()))==1,"AES operation failed");
  if(!encrypt)require(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_TAG,16,&tag[0])==1,"AES tag failed");
  if(EVP_CipherFinal_ex(ctx.get(),reinterpret_cast<unsigned char*>(&output[0])+written,&n)!=1){wipe(output);throw std::runtime_error("Wrong password or damaged encrypted pack");}output.resize(static_cast<size_t>(written+n));
  if(encrypt)require(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_GET_TAG,16,&tag[0])==1,"AES tag failed");
#endif
  return output;
}
static bool safe_name(const std::string& name){
  if(fs::u8path(name).lexically_normal().generic_u8string()!=name)return false;
  if(name.empty()||name.size()>1024||name.front()=='/'||name.back()=='/'||name.find_first_of("\\:")!=std::string::npos)return false;
  for(unsigned char c:name)if(c<32||c==127)return false;
  for(const auto& p:fs::u8path(name)){auto part=p.u8string();if(part.empty()||part=="."||part==".."||part.back()=='.'||part.back()==' ')return false;for(char& c:part)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));part=part.substr(0,part.find('.'));if(part=="con"||part=="prn"||part=="aux"||part=="nul"||(part.size()==4&&(part.rfind("com",0)==0||part.rfind("lpt",0)==0)&&part[3]>='1'&&part[3]<='9'))return false;}
  return true;
}
static void integer(std::string& out,uint32_t value){for(int i=0;i<4;++i)out+=static_cast<char>((value>>(i*8))&255);}
static void field(std::string& out,const std::string& value){require(value.size()<=limit,"Pack field too large");integer(out,static_cast<uint32_t>(value.size()));out+=value;require(out.size()<=limit,"Pack exceeds 128 MiB");}
static void validate(const Files& files){require(files.size()<=4096,"Too many pack files");std::set<std::string> names;for(const auto& item:files){require(safe_name(item.first),"Unsafe pack path");auto name=item.first;for(char& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));require(names.insert(name).second,"Case-colliding pack paths");}for(const auto& name:names){auto p=fs::u8path(name).parent_path();while(!p.empty()){require(!names.count(p.generic_u8string()),"File/directory collision in pack");p=p.parent_path();}}}
static std::string encode(const std::string& kind,const Files& files,const std::string& password,const Modes& executable={}){validate(files);std::string payload;field(payload,kind);integer(payload,static_cast<uint32_t>(files.size()));for(const auto& item:files){field(payload,item.first);integer(payload,executable.count(item.first)?1:0);field(payload,item.second);}if(password.empty())return "CXPACK1P"+native_web::digest(payload)+payload;auto salt=random_bytes(16),nonce=random_bytes(12),tag=std::string(16,'\0');auto encrypted=crypt(true,payload,password,salt,nonce,tag);wipe(payload);return "CXPACK1E"+salt+nonce+tag+encrypted;}
static Files decode(const std::string& bytes,const std::string& password,const std::string& expected,Modes* executable=nullptr){
  require(bytes.size()>=8&&bytes.size()<=limit+128,"Invalid pack size");std::string payload;
  if(bytes.substr(0,8)=="CXPACK1P"){require(password.empty(),"Password supplied for an unauthenticated plaintext pack");require(bytes.size()>=72,"Truncated pack");payload=bytes.substr(72);require(native_web::digest(payload)==bytes.substr(8,64),"Pack checksum mismatch");}
  else if(bytes.substr(0,8)=="CXPACK1E"){require(bytes.size()>=52,"Truncated encrypted pack");auto tag=bytes.substr(36,16);payload=crypt(false,bytes.substr(52),password,bytes.substr(8,16),bytes.substr(24,12),tag);}
  else throw std::runtime_error("Unknown pack format");
  struct Clear{std::string& value;~Clear(){wipe(value);}} clear{payload};size_t offset=0;
  auto number=[&](){require(payload.size()-offset>=4,"Truncated pack field");uint32_t n=0;for(int i=0;i<4;++i)n|=static_cast<uint32_t>(static_cast<unsigned char>(payload[offset++]))<<(i*8);return n;};
  auto text=[&](){auto n=number();require(n<=payload.size()-offset,"Truncated pack content");auto out=payload.substr(offset,n);offset+=n;return out;};
  require(text()==expected,"Wrong pack kind");const auto count=number();require(count<=4096,"Too many pack files");Files files;for(uint32_t i=0;i<count;++i){auto name=text();auto mode=number();require(mode<=1,"Invalid pack file mode");if(mode&&executable)executable->insert(name);auto data=text();require(files.emplace(std::move(name),std::move(data)).second,"Duplicate pack path");}require(offset==payload.size(),"Trailing pack content");validate(files);return files;
}
static void publish_file(const fs::path& from,const fs::path& to){
#if defined(_WIN32)
  require(MoveFileExW(from.c_str(),to.c_str(),0)!=0,"Output already exists or cannot be published");
#else
  require(::link(from.c_str(),to.c_str())==0,"Output already exists or cannot be published");require(::unlink(from.c_str())==0,"Cannot clear staged pack");
#endif
}
static void publish_directory(const fs::path& from,const fs::path& to){
#if defined(_WIN32)
  require(MoveFileExW(from.c_str(),to.c_str(),0)!=0,"Destination exists or cannot be published");
#elif defined(__APPLE__)
  require(::renamex_np(from.c_str(),to.c_str(),RENAME_EXCL)==0,"Destination exists or cannot be published");
#else
  require(::syscall(SYS_renameat2,AT_FDCWD,from.c_str(),AT_FDCWD,to.c_str(),1u)==0,"Destination exists or atomic publish unavailable");
#endif
}
struct Stage{fs::path path;explicit Stage(const fs::path& parent){fs::create_directories(parent);path=parent/random_id(".codex-transfer");require(fs::create_directory(path),"Cannot create transfer staging");native_web::private_dir(path);}~Stage(){std::error_code ec;fs::remove_all(path,ec);}};
static void save_new(const fs::path& path,const std::string& data){require(!fs::exists(path),"Output already exists");Stage stage(path.parent_path());std::string error;require(write_all(stage.path/"pack",data,error),error);native_web::protect(stage.path/"pack");
#if !defined(_WIN32)
  fs::permissions(stage.path/"pack",fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
#endif
  publish_file(stage.path/"pack",path);
}
static Files collect(const fs::path& root,const std::function<bool(const std::string&)>& include){Files out;size_t total=0;for(const auto& entry:fs::recursive_directory_iterator(root)){auto rel=entry.path().lexically_relative(root).generic_u8string();auto status=entry.symlink_status();require(!fs::is_symlink(status),"Linked portable file denied");if(fs::is_directory(status))continue;require(fs::is_regular_file(status),"Special portable file denied");if(!include(rel))continue;require(out.size()<4096,"Too many portable files");auto bytes=creator_read(root,rel,32*1024*1024);total+=bytes.size();require(total<=limit,"Portable data exceeds 128 MiB");out.emplace(rel,std::move(bytes));}return out;}
static Modes modes(const fs::path& root,const Files& files){Modes out;for(const auto& item:files)if((fs::status(root/fs::u8path(item.first)).permissions()&fs::perms::owner_exec)!=fs::perms::none)out.insert(item.first);return out;}
static void extract(const fs::path& stage,const Files& files,const Modes& executable={}){validate(files);for(const auto& item:files){auto file=stage/fs::u8path(item.first);fs::create_directories(file.parent_path());
#if defined(_WIN32)
  HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(h!=INVALID_HANDLE_VALUE,"Duplicate or invalid extracted file");DWORD written=0;bool ok=WriteFile(h,item.second.data(),static_cast<DWORD>(item.second.size()),&written,nullptr)&&written==item.second.size();CloseHandle(h);require(ok,"Cannot extract file");native_web::protect(file);
#else
  int fd=::open(file.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);require(fd>=0,"Duplicate or invalid extracted file");size_t offset=0;while(offset<item.second.size()){auto n=::write(fd,item.second.data()+offset,item.second.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0){::close(fd);throw std::runtime_error("Cannot extract file");}offset+=static_cast<size_t>(n);}::close(fd);fs::permissions(file,executable.count(item.first)?fs::perms::owner_all:fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
#endif
}}

struct Lease{
#if defined(_WIN32)
  HANDLE handle=INVALID_HANDLE_VALUE;OVERLAPPED overlap{};
#else
  int handle=-1;
#endif
  Lease(const fs::path& directory,bool exclusive,const char* filename=".runtime.lock",const char* busy="Portable data is in use; stop other instances/tasks first"){native_web::private_dir(directory);auto path=directory/filename;
#if defined(_WIN32)
    handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);BY_HANDLE_FILE_INFORMATION info{};if(handle==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nNumberOfLinks!=1||!LockFileEx(handle,LOCKFILE_FAIL_IMMEDIATELY|(exclusive?LOCKFILE_EXCLUSIVE_LOCK:0),0,1,0,&overlap)){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);handle=INVALID_HANDLE_VALUE;throw std::runtime_error(busy);}
#else
    handle=::open(path.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);struct stat info{};if(handle<0||::fstat(handle,&info)||!S_ISREG(info.st_mode)||info.st_nlink!=1||flock(handle,(exclusive?LOCK_EX:LOCK_SH)|LOCK_NB)){if(handle>=0)::close(handle);handle=-1;throw std::runtime_error(busy);}
#endif
  }
  ~Lease(){
#if defined(_WIN32)
    if(handle!=INVALID_HANDLE_VALUE){UnlockFileEx(handle,0,1,0,&overlap);CloseHandle(handle);}
#else
    if(handle>=0)::close(handle);
#endif
  }
  Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
};
static std::unique_ptr<Lease> runtime_lease;
static void activate(){if(portable_config.root.empty())return;native_web::private_dir(portable_config.root);native_web::private_dir(portable_config.root/"user");runtime_lease=std::make_unique<Lease>(portable_config.root,false);process_protected_paths.push_back(fs::canonical(portable_config.root));}
static std::string password_env(const char* name){
#if defined(_WIN32)
  auto wide=fs::u8path(name).wstring();DWORD size=GetEnvironmentVariableW(wide.c_str(),nullptr,0);if(!size)return {};std::wstring value(size,L'\0');DWORD n=GetEnvironmentVariableW(wide.c_str(),value.data(),size);require(n&&n<size,"Cannot read password environment");int count=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(n),nullptr,0,nullptr,nullptr);std::string out(count,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(n),out.data(),count,nullptr,nullptr);SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));return out;
#else
  return getenv_or(name);
#endif
}
static std::string password_option(int argc,char** argv,int start,bool& credentials){std::string password;for(int i=start;i<argc;++i){std::string arg=argv[i];if(arg=="--include-credentials")credentials=true;else if(arg=="--password-env"&&i+1<argc){password=password_env(argv[++i]);require(!password.empty(),"Password environment variable is empty");}else throw std::runtime_error("Unknown transfer option");}require(!credentials||!password.empty(),"Credential export requires --password-env NAME");return password;}
static std::string folded(std::string name){for(char& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return name;}
static bool profile_path(const std::string& name){if(name.rfind("projects/",0)==0)return true;if(name.rfind("user/",0)!=0)return false;const auto first=name.substr(5,name.find('/',5)-5);return std::all_of(first.begin(),first.end(),[](unsigned char c){return c<128;});}
static int cli(int argc,char** argv){try{if(argc==3&&std::string(argv[2])=="capabilities"){std::cout<<"{\"application\":\"codex-cpp\",\"format\":1,\"update_protocol\":1}\n";return 0;}require(argc>=3,"Usage: --portable-dir DIR portable status | export FILE [--include-credentials] [--password-env NAME] | import FILE [--password-env NAME]");require(!portable_config.root.empty(),"Choose --portable-dir DIR first");const auto root=portable_config.root;require(root!=root.root_path(),"Do not use a filesystem root as a portable directory");std::string action=argv[2];
  if(action=="status"&&argc==3){std::cout<<json_dump(plugin_object({{"root",plugin_string(root.u8string())},{"home",plugin_string((root/"user").u8string())},{"project",plugin_string(portable_project_key(fs::current_path()))},{"at_rest_encrypted",plugin_bool(false)}}))<<'\n';return 0;}
  require(argc>=4,"A pack filename is required");auto pack=fs::absolute(fs::u8path(argv[3]));bool credentials=false;auto password=password_option(argc,argv,4,credentials);struct Clear{std::string& value;~Clear(){wipe(value);}} clear{password};
  if(action=="export"){require(fs::is_directory(root),"Portable directory does not exist");require(!within_root(root,pack),"Export outside the portable directory");Lease lease(root,true);auto files=collect(fs::canonical(root),[&](const std::string& name){return profile_path(name)&&(credentials||name.rfind("projects/",0)==0||name.rfind("user/plugins/",0)==0);});files.erase("user/plugins-enabled.json");save_new(pack,encode("profile",files,password,modes(root,files)));std::cout<<"Exported "<<files.size()<<" files; credentials "<<(credentials?"included in encrypted pack":"account/provider configuration excluded; chat/plugin files may still contain secrets")<<"\n";return 0;}
  if(action=="import"){require(!credentials,"--include-credentials is export-only");require(!fs::exists(root),"Import requires a new destination directory; existing data is never overwritten");Modes executable;auto packed=native_web::read_bytes(pack,limit+128);auto files=decode(packed,password,"profile",&executable);for(const auto& item:files){require(profile_path(item.first),"Unexpected profile path");if(packed.substr(0,8)=="CXPACK1P")require(item.first.rfind("projects/",0)==0||item.first.rfind("user/plugins/",0)==0,"Account/provider files require an encrypted profile");}for(auto it=files.begin();it!=files.end();){const auto name=folded(it->first);if(name=="user/plugins-enabled.json"){executable.erase(it->first);it=files.erase(it);}else ++it;}files["user/plugins-enabled.json"]="[]\n";std::string mcp;for(const auto& item:files)if(folded(item.first)=="user/mcp.json")mcp=item.first;if(!mcp.empty()){for(const auto& item:files)require(folded(item.first)!="user/mcp.imported.json","MCP quarantine path already exists");files["user/mcp.imported.json"]=files.at(mcp);files.erase(mcp);executable.erase(mcp);}Stage stage(root.parent_path());extract(stage.path,files,executable);publish_directory(stage.path,root);std::cout<<"Imported portable data; plugins disabled; credentials may require login on this device\n";return 0;}
  throw std::runtime_error("Unknown portable action");
}catch(const std::exception& e){std::cerr<<"Portable: "<<e.what()<<'\n';return 2;}}
// Sharing carries declared libraries for other platforms without loading them.
static Files shared_files(const fs::path& root){Files files;creator_check(root,root.u8string(),&files);auto manifest=plugin_json(files.at("plugin.json"));require(bool(manifest),"Invalid plugin manifest");
  if(auto libraries=manifest->get("libraries")){require(libraries->kind==Json::Object,"Invalid library declarations");for(const auto& item:libraries->fields){require(item.second.kind==Json::String&&safe_name(item.second.str()),"Unsafe declared library path");const auto name=item.second.str();if(fs::exists(root/fs::u8path(name)))files[name]=creator_read(root,name,32*1024*1024);}}
  validate(files);return files;
}
static int plugin_share(int argc,char** argv){try{require(argc==5,"Usage: plugin export BUNDLE PACK | plugin import PACK NEW_DIRECTORY");std::string action=argv[2];auto source=fs::absolute(fs::u8path(argv[3])),dest=fs::absolute(fs::u8path(argv[4]));
  if(action=="export"){if(fs::is_regular_file(source))source=source.parent_path();auto files=shared_files(fs::canonical(source));save_new(dest,encode("plugin",files,{},modes(source,files)));std::cout<<"Plugin pack exported; checksum is not a publisher signature\n";return 0;}
  require(action=="import","Unknown plugin transfer action");require(!fs::exists(dest),"Plugin destination already exists");Modes executable;auto files=decode(native_web::read_bytes(source,limit+128),{},"plugin",&executable);require(files.count("plugin.json")!=0,"Plugin manifest missing");Stage stage(dest.parent_path());extract(stage.path,files,executable);auto checked=shared_files(fs::canonical(stage.path));require(files==checked,"Plugin contains undeclared files");publish_directory(stage.path,dest);std::cout<<"Plugin imported, not enabled. Review, rebuild for this platform if necessary, and explicitly enable after approval.\n";return 0;
}catch(const std::exception& e){std::cerr<<"Plugin transfer: "<<e.what()<<'\n';return 2;}}
} // namespace portable
