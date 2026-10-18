// Native Windows CI: no login, no model calls, no desktop input injection.
#define main codex_runtime_main
#include "../codex.cpp"
#undef main
#include <cassert>
#if defined(_WIN32)
struct ConsoleBuffer:std::streambuf{HANDLE output;explicit ConsoleBuffer(HANDLE h):output(h){}std::streamsize xsputn(const char* s,std::streamsize n)override{DWORD written=0;return WriteFile(output,s,static_cast<DWORD>(n),&written,nullptr)?written:0;}int overflow(int c)override{if(c==traits_type::eof())return traits_type::not_eof(c);char ch=static_cast<char>(c);return xsputn(&ch,1)==1?c:traits_type::eof();}};
int main(){try{
  const auto root=fs::temp_directory_path()/random_id("windows-runtime");fs::create_directories(root);execution.initialize(root);
  std::string error;ManagedProcess denied;ProcessPolicy restricted=ProcessPolicy::tool(root,true);
  if(denied.start(shell_argv("echo forbidden>forbidden.txt"),restricted,error))throw std::runtime_error("restricted process started without a sandbox");
  if(fs::exists(root/"forbidden.txt")||error.find("DENIED")==std::string::npos)throw std::runtime_error("missing fail-closed denial");
  _putenv_s("OPENAI_API_KEY","DO_NOT_INHERIT");auto unsafe=restricted;unsafe.unsafe=true;
  const auto environment=run_capture("set",8000,unsafe);
  if(environment.exit_code||environment.output.find("DO_NOT_INHERIT")!=std::string::npos||environment.output.find("SystemRoot=")==std::string::npos)throw std::runtime_error("Windows child environment invalid");
  auto output=run_capture(cd_prefix(root)+"echo WINDOWS_CHILD_OK",8000,unsafe);
  if(output.exit_code||output.output.find("WINDOWS_CHILD_OK")==std::string::npos)throw std::runtime_error("native child execution failed: "+output.output);
  std::cout<<"PASS Windows fail-closed policy and explicit unsafe child environment/pipes\n"<<std::flush;
  const auto original_in=GetStdHandle(STD_INPUT_HANDLE),original_out=GetStdHandle(STD_OUTPUT_HANDLE);
  // A private console tests native records without taking over any user's console.
  FreeConsole();if(!AllocConsole())throw std::runtime_error("AllocConsole failed");
  HANDLE input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
  HANDLE screen=CreateFileW(L"CONOUT$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
  if(input==INVALID_HANDLE_VALUE||screen==INVALID_HANDLE_VALUE)throw std::runtime_error("private console handles failed");
  SetStdHandle(STD_INPUT_HANDLE,input);SetStdHandle(STD_OUTPUT_HANDLE,screen);DWORD output_mode=0;GetConsoleMode(screen,&output_mode);
  if(!SetConsoleMode(screen,output_mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING))throw std::runtime_error("VT console unavailable");
  DWORD input_mode=0;GetConsoleMode(input,&input_mode);const auto input_cp=GetConsoleCP(),output_cp=GetConsoleOutputCP();
  ConsoleBuffer buffer(screen);auto old_buffer=std::cout.rdbuf(&buffer);
  {
    TuiRenderer tui(root,"fixture","fixture","workspace-write");tui.external_mode("fixture",true);FlushConsoleInputBuffer(input);
    auto key=[&](WORD vk,WCHAR ch,DWORD modifiers,const std::string& expected,const std::string& text=""){
      INPUT_RECORD r{};r.EventType=KEY_EVENT;r.Event.KeyEvent={TRUE,1,vk,0,{ch},modifiers};DWORD written=0;
      if(!WriteConsoleInputW(input,&r,1,&written)||written!=1)throw std::runtime_error("cannot enqueue console record");
      auto decoded=tui.external_key();if(!decoded||decoded->first!=expected||decoded->second!=text)throw std::runtime_error("native key mismatch: "+expected);
    };
    key('A',L'a',0,"text","a");key(0,L'中',0,"text",u8"中");
    key(VK_LEFT,0,0,"left");key(VK_RIGHT,0,0,"right");key(VK_RETURN,L'\r',0,"enter");key(VK_RETURN,L'\r',SHIFT_PRESSED,"newline");
    key('C',3,LEFT_CTRL_PRESSED,"host.stop");key('G',7,LEFT_CTRL_PRESSED,"host.menu");
    INPUT_RECORD resize{};resize.EventType=WINDOW_BUFFER_SIZE_EVENT;resize.Event.WindowBufferSizeEvent.dwSize={80,24};DWORD n=0;WriteConsoleInputW(input,&resize,1,&n);
    auto resized=tui.external_key();if(!resized||resized->first!="resize")throw std::runtime_error("resize event missing");
    tui.external_frame({"NATIVE_RENDER_OK"},0,0);WCHAR row[32]{};DWORD read=0;
    if(!ReadConsoleOutputCharacterW(screen,row,16,{0,0},&read)||std::wstring(row,read)!=L"NATIVE_RENDER_OK")throw std::runtime_error("native VT frame did not render");
  }
  std::cout.rdbuf(old_buffer);DWORD restored=0;GetConsoleMode(input,&restored);
  const bool restored_ok=restored==input_mode&&GetConsoleCP()==input_cp&&GetConsoleOutputCP()==output_cp;
  SetStdHandle(STD_INPUT_HANDLE,original_in);SetStdHandle(STD_OUTPUT_HANDLE,original_out);CloseHandle(input);CloseHandle(screen);FreeConsole();
  if(!restored_ok)throw std::runtime_error("console input mode/codepage was not restored");
  fs::remove_all(root);std::cout<<"PASS Windows native Unicode/key/resize input, VT frame and console restoration\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
#else
int main(){return 77;}
#endif
