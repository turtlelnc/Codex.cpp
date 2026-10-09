// Included by codex.cpp after its shared types and helpers.
// ---------- terminal UI ----------
enum class UiMode{Auto,Tui,Plain};static bool terminal_is_tty(){
#if defined(_WIN32)
return _isatty(_fileno(stdin))&& _isatty(_fileno(stdout));
#else
return isatty(STDIN_FILENO)&& isatty(STDOUT_FILENO);
#endif
}
static bool enable_and_detect_vt(){if(!terminal_is_tty())return false;
#if defined(_WIN32)
HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);if(out == INVALID_HANDLE_VALUE || out == nullptr)return false;DWORD mode = 0; if(!GetConsoleMode(out,&mode))return false;if(!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)){
  if(!SetConsoleMode(out,mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))return false;
}
return true;
#else
const std::string term = getenv_or("TERM");if(term.empty()|| term == "dumb")return false;return true;
#endif
}
static size_t terminal_columns(){
#if defined(_WIN32)
CONSOLE_SCREEN_BUFFER_INFO info{};HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);if(out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out,&info)){
  const int width = info.srWindow.Right - info.srWindow.Left + 1;if(width > 0)return static_cast<size_t>(width);
}
#else
winsize ws{};if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)== 0 && ws.ws_col > 0)return static_cast<size_t>(ws.ws_col);
if(const char* cols = std::getenv("COLUMNS")){try{const int n = std::stoi(cols);if(n > 0)return static_cast<size_t>(n);}catch(...){} }
#endif
return 80; } static size_t terminal_rows(){
#if defined(_WIN32)
CONSOLE_SCREEN_BUFFER_INFO info{};HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);if(out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out,&info)){
  const int height = info.srWindow.Bottom - info.srWindow.Top + 1;if(height > 10)return static_cast<size_t>(height);
}
#else
winsize ws{};if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)== 0 && ws.ws_row > 10)return ws.ws_row;if(const char* rows = std::getenv("LINES")){
  try{const int n = std::stoi(rows);if(n > 10)return static_cast<size_t>(n);}catch(...){}
}
#endif
return 32; } struct SlashCommandDef{const char* name;const char* description;bool implemented; }; static const std::vector<SlashCommandDef>& slash_commands(){static const std::vector<SlashCommandDef> kCommands ={
  {"/model","choose what model and reasoning effort to use",true},{"/login","sign in with ChatGPT device code",true},
  {"/auth","show ChatGPT login status",true},{"/ide","include current selection, open files, and other context from your IDE",false},
  {"/permissions","choose what Codex is allowed to do",true},{"/keymap","remap TUI shortcuts",false},{"/vim","toggle Vim mode for the composer",false},
  {"/experimental","toggle experimental features",false},{"/approve","approve one retry of a recent auto-review denial",false},
  {"/skills","list available project and user skills",true},{"/import","import setup, this project, and recent chats from Claude Code",false},
  {"/hooks","view and manage lifecycle hooks",false},{"/review","review my current changes and find issues",true},
  {"/rename","rename the current thread",true},{"/new","start a new chat during a conversation",true},{"/archive","archive this session and exit",true},
  {"/delete","permanently delete this session and exit",true},{"/resume","resume a saved chat",true},{"/fork","fork the current chat",true},
  {"/continue","continue the unfinished turn",true},{"/memory","view project memory or append a note",true},
  {"/app","continue this session in the Desktop app",false},{"/init","create an AGENTS.md file with instructions for Codex",true},
  {"/compact","summarize conversation to prevent hitting the context limit",true},{"/plan","switch to Plan mode",true},
  {"/goal","set or view the goal for a long-running task",true},{"/agent","switch the active agent thread",false},
  {"/copy","copy last response as markdown",true},
  {"/raw","toggle raw scrollback mode for copy-friendly terminal selection",false},{"/diff","show git diff (including untracked files)",true},
  {"/mention","mention a file",true},{"/image","attach a PNG, JPEG, WebP or GIF to the next turn",true},
  {"/status","show current session configuration and token usage",true},{"/usage","view account usage or use a usage limit reset",false},
  {"/title","configure which items appear in the terminal title",true},{"/statusline","configure which items appear in the status line",true},
  {"/theme","choose a syntax highlighting theme",false},{"/pets","choose or hide the terminal pet",false},
  {"/mcp","list connected local MCP stdio servers and tools",true},{"/plugins","browse plugins",false},{"/logout","log out of Codex",true},
  {"/feedback","send logs to maintainers",false},{"/ps","list background terminals",true},
  {"/stop","stop all background terminals",true},
  {"/personality","choose a communication style for Codex",true},{"/subagents","switch the active agent thread",false}, {"/memories","configure memory use and generation",false},
  // C++ port helpers. Hidden from the default popup but still accepted.
  {"/help","show available commands",true},{"/sessions","list local saved sessions",true},{"/history","show input history",true}, {"/quit","exit Codex",true},
};
return kCommands; } static const SlashCommandDef* find_slash_command(const std::string& name){for(const auto& command : slash_commands())if(name == command.name)return &command; return nullptr; }
static std::string slash_command_token(const std::string& input){if(input.empty()|| input.front()!= '/')return{};
const size_t end = input.find_first_of(" \t\r\n");return input.substr(0,end == std::string::npos ? input.size(): end); }
static std::vector<const SlashCommandDef*> matching_slash_commands(const std::string& input){std::vector<const SlashCommandDef*> out;
// Popup is valid only for a command token that starts at byte/column zero.
// Once whitespace/newline appears, the user is entering arguments, not filtering commands.
if(input.empty()|| input.front()!= '/' || input.find_first_of(" \t\r\n")!= std::string::npos)return out;
const std::string filter = slash_command_token(input);if(filter.empty())return out;for(const auto& command : slash_commands()){
  const std::string name = command.name;if(name == "/help" || name == "/sessions" || name == "/history" || name == "/quit")continue; if(name.rfind(filter,0)== 0)out.push_back(&command);
}
return out; } class TuiRenderer{public: enum class CellKind{User,Assistant,Tool,Output,Patch,Error,System,SessionSummary,Notice};
struct Cell{CellKind kind;std::string title;std::string body;};TuiRenderer(const fs::path& root,std::string provider,std::string model,std::string sandbox)
  : root_(root.string()),provider_(std::move(provider)),model_(std::move(model)),sandbox_(std::move(sandbox)){
  std::cout << "\x1b[?1049h\x1b[?2004h\x1b[?1000h\x1b[?1006h\x1b[?25l\x1b[2J\x1b[H" << std::flush;active_ = true; cells_.push_back({CellKind::SessionSummary,"Codex C++",""});
  cells_.push_back({CellKind::Notice,"","Tip: Type /help for commands, or start by describing a coding task."});render();
}
~TuiRenderer(){if(active_)std::cout << "\x1b[?25h\x1b[?1006l\x1b[?1000l\x1b[?2004l\x1b[?1049l" << std::flush; } TuiRenderer(const TuiRenderer&)= delete;TuiRenderer& operator=(const TuiRenderer&)= delete;
void add_user(const std::string& text){add_cell(CellKind::User,"",text);}void add_tool(const std::string& name,const std::string& args ={}){
  std::string label;if(name == "shell")label = "Ran shell command";else if(name == "read_file")label = "Read file";
  else if(name == "list_dir")label = "Explored directory";else if(name == "write_file")label = "Wrote file";
  else if(name == "apply_patch")label = "Applied patch";else label = name;add_cell(CellKind::Tool,label,compact_args(args));
}
void add_tool_output(const std::string& text){std::string shown = text;if(shown.size()> 5000)shown = shown.substr(0,5000)+ "\n[output truncated]"; add_cell(CellKind::Output,"",shown);
}
void add_diff(const std::string& patch){add_cell(CellKind::Patch,"Edited files",patch);} void add_assistant(const std::string& text){add_cell(CellKind::Assistant,"",text);}
void add_error(const std::string& text){add_cell(CellKind::Error,"Error",text);}void add_system(const std::string& text){add_cell(CellKind::System,"",text);}
void reset_transcript(){cells_.clear();scroll_offset_=0; cells_.push_back({CellKind::SessionSummary,"Codex C++",""});
  cells_.push_back({CellKind::Notice,"","Tip: Type /help for commands, or start by describing a coding task."});render();
}
void set_model(std::string model,std::string effort ={}){model_=std::move(model);effort_=std::move(effort);render();}
void set_sandbox(std::string sandbox){sandbox_=std::move(sandbox);render();}void set_thread_name(std::string name){thread_name_=std::move(name);render();}
void set_statusline_items(const std::vector<std::string>& items){footer_model_=footer_cwd_=footer_thread_=footer_sandbox_=false;for(const auto& item:items){
  if(item=="model")footer_model_=true;else if(item=="cwd"||item=="directory")footer_cwd_=true; else if(item=="thread"||item=="name")footer_thread_=true;else if(item=="sandbox"||item=="permissions")footer_sandbox_=true; }
  render();
}
int choose(const std::string& title,const std::vector<std::string>& options,size_t selected=0){if(options.empty())return -1;
  overlay_active_=true;overlay_title_=title;overlay_options_=options;overlay_choice_=static_cast<int>(std::min(selected,options.size()-1));render();
#if !defined(_WIN32)
  termios oldt{};bool raw_ok=tcgetattr(STDIN_FILENO,&oldt)==0;
  if(raw_ok){termios raw=oldt;raw.c_lflag&=static_cast<tcflag_t>(~(ICANON|ECHO));raw.c_iflag&=static_cast<tcflag_t>(~(IXON|ICRNL));tcsetattr(STDIN_FILENO,TCSANOW,&raw);}
#endif
  int result=-1;for(;;){int k=read_key();
#if !defined(_WIN32)
  if(tui_interrupt_requested){result=-1;break;}
#endif
  if(k==27){result=-1;break;} if(k==KEY_UP){overlay_choice_=(overlay_choice_+static_cast<int>(options.size())-1)%static_cast<int>(options.size());render();continue;}
  if(k==KEY_DOWN){overlay_choice_=(overlay_choice_+1)%static_cast<int>(options.size());render();continue;}
  if(k>='1' && k<='9'){int n=k-'1';if(n<static_cast<int>(options.size())){result=n;break;}}if(k=='\r'||k=='\n'){result=overlay_choice_;break;} }
#if !defined(_WIN32)
  if(raw_ok)tcsetattr(STDIN_FILENO,TCSANOW,&oldt);
#endif
  overlay_active_=false;overlay_options_.clear();overlay_body_.clear();render();return result;
}
bool approval(const std::string& text){overlay_body_=text;const int choice=choose("Would you like to run this command?",{"Yes, proceed","No, cancel"},0); overlay_body_.clear();return choice==0;
}
void render(const std::string& footer = ""){status_ = footer; if(!footer.empty()&&(footer.find("working")!= std::string::npos || footer.find("running")!= std::string::npos)){
  if(!working_){working_=true;working_started_=std::chrono::steady_clock::now();} }else if(footer.empty()){ working_=false; } draw_diff(build_frame());
}
void append_assistant_delta(const std::string& delta){if(delta.empty())return;if(!assistant_stream_active_){add_cell(CellKind::Assistant,"",""); assistant_stream_index_=cells_.size()-1;assistant_stream_active_=true; }
  cells_[assistant_stream_index_].body+=delta;draw_diff(build_frame());
}
void finish_assistant_stream(const std::string& text){if(assistant_stream_active_)cells_[assistant_stream_index_].body=text;
  else if(!text.empty())add_cell(CellKind::Assistant,"",text);assistant_stream_active_=false;working_=false;status_.clear();draw_diff(build_frame());
}
void stream_assistant(const std::string& text){finish_assistant_stream(text);}std::string read_line(std::vector<std::string>& history){
#if defined(_WIN32)
  std::cout << "\x1b[?25h" << std::flush;std::string line;std::getline(std::cin,line);std::cout << "\x1b[?25l" << std::flush;return line;
#else
  termios oldt{};if(tcgetattr(STDIN_FILENO,&oldt)!= 0){std::string line;std::getline(std::cin,line);return line;}
  termios raw=oldt;raw.c_lflag &= static_cast<tcflag_t>(~(ICANON|ECHO));raw.c_iflag &= static_cast<tcflag_t>(~(IXON|ICRNL));
  tcsetattr(STDIN_FILENO,TCSANOW,&raw);std::string line;size_t cursor=0,hist=history.size();bool popup=false;
  composer_="";composer_cursor_=0;command_popup_selected_=0;working_=false;render();for(;;){int k=read_key();
#if !defined(_WIN32)
  if(tui_interrupt_requested){line.clear();break;}
#endif
  if(k==KEY_IGNORED)continue;if(k==KEY_PASTE){
    // Treat the whole bracketed paste as text. In particular, its
    // newlines must not submit the prompt or select a slash item.
    line.insert(cursor,pasted_text_);cursor+=pasted_text_.size();pasted_text_.clear();popup=false; composer_=line;composer_cursor_=cursor;render();continue;
  }
  // Enter submits. Ctrl+J inserts a real newline. Enhanced terminals may
  // report Shift+Enter as KEY_NEWLINE as well.
  if(k=='\r'){auto matches = matching_slash_commands(line);if(!matches.empty()){
    command_popup_selected_ = std::min(command_popup_selected_,matches.size()-1);line = matches[command_popup_selected_]->name;cursor = line.size();
    // A selected bare slash command is accepted immediately, matching Codex's popup behavior.
    break; } if(!line.empty())break;else continue;
  }
  if(k=='\n' || k==KEY_NEWLINE){line.insert(cursor,1,'\n');++cursor;}else if(k==3){line.clear();break;}
  else if(k==27){if(popup){popup=false;render();continue;}line.clear();break;}else if(k==127 || k==8){
    if(cursor>0){size_t prev=prev_utf8_boundary(line,cursor);line.erase(prev,cursor-prev);cursor=prev;}
  }
  else if(k==KEY_DELETE){if(cursor<line.size()){size_t next=next_utf8_boundary(line,cursor);line.erase(cursor,next-cursor);} } else if(k==KEY_LEFT){if(cursor>0)cursor=prev_utf8_boundary(line,cursor);}
  else if(k==KEY_RIGHT){if(cursor<line.size())cursor=next_utf8_boundary(line,cursor);}else if(k==KEY_HOME)cursor=line_start_boundary(line,cursor);
  else if(k==KEY_END)cursor=line_end_boundary(line,cursor);else if(k==KEY_UP){auto matches = matching_slash_commands(line);if(!matches.empty()){
    if(command_popup_selected_ == 0)command_popup_selected_ = matches.size()-1;else --command_popup_selected_; }else{
    const size_t text_width=current_composer_text_width();if(!move_cursor_vertical(line,cursor,-1,text_width)&& !history.empty()){ if(hist>0)--hist;line=history[hist];cursor=line.size();
    }
    }
  }
  else if(k==KEY_DOWN){auto matches = matching_slash_commands(line);if(!matches.empty()){ command_popup_selected_ =(command_popup_selected_ + 1)% matches.size(); }else{
    const size_t text_width=current_composer_text_width();if(!move_cursor_vertical(line,cursor,+1,text_width)&& !history.empty()){
      if(hist<history.size())++hist;line=hist<history.size()?history[hist]:"";cursor=line.size();
    }
    }
  }
  else if(k==KEY_PGUP){scroll_offset_ += 4;}else if(k==KEY_PGDN){scroll_offset_ = scroll_offset_>4?scroll_offset_-4:0;}
  else if(k==KEY_SCROLL_UP){++scroll_offset_;}else if(k==KEY_SCROLL_DOWN){if(scroll_offset_)--scroll_offset_;}else if(k>=32 && k<=255){
    // Terminal input is a byte stream.  For non-ASCII text (notably IME
    // commits on macOS) consume the entire UTF-8 code point before
    // mutating/re-rendering the composer. Rendering after each byte of a
    // multi-byte character produces transient invalid UTF-8 and visible
    // blank/flicker artifacts during composition commit.
    if(k < 0x80){line.insert(line.begin()+static_cast<long>(cursor),static_cast<char>(k));++cursor; }else{ const unsigned char first = static_cast<unsigned char>(k);int expected = 1;if((first & 0xE0)== 0xC0)expected = 2;
    else if((first & 0xF0)== 0xE0)expected = 3;else if((first & 0xF8)== 0xF0)expected = 4;std::string cp;cp.push_back(static_cast<char>(first));
#if !defined(_WIN32)
    for(int i=1;i<expected;++i){unsigned char cont=0;if(::read(STDIN_FILENO,&cont,1)!=1)break;if((cont & 0xC0)!= 0x80)break; cp.push_back(static_cast<char>(cont));
    }
#endif
    line.insert(cursor,cp);cursor += cp.size(); }
  }
  // Derive popup visibility from the current composer contents on every key.
  // It is never allowed to survive as stale UI state.
  auto matches = matching_slash_commands(line);popup = !matches.empty();if(!popup)command_popup_selected_ = 0; else if(command_popup_selected_ >= matches.size())command_popup_selected_ = matches.size()-1;
  composer_=line;composer_cursor_=cursor;render(); } tcsetattr(STDIN_FILENO,TCSANOW,&oldt);composer_.clear();composer_cursor_=0;render();return line;
#endif
}
void prompt(){working_=false;status_.clear();render();} private: static constexpr int KEY_LEFT=1001,KEY_RIGHT=1002,KEY_UP=1003,KEY_DOWN=1004,KEY_HOME=1005,KEY_END=1006,KEY_PGUP=1007,KEY_PGDN=1008,
      KEY_DELETE=1009,KEY_NEWLINE=1010,KEY_PASTE=1011,KEY_IGNORED=1012,KEY_SCROLL_UP=1013,KEY_SCROLL_DOWN=1014;
std::string pasted_text_;int read_key(){
#if defined(_WIN32)
  return std::cin.get();
#else
  unsigned char c=0;if(::read(STDIN_FILENO,&c,1)!=1)return -1;if(c!=27)return c;unsigned char a=0;if(::read(STDIN_FILENO,&a,1)!=1)return 27;
  // Option/Alt+Enter is treated as newline on terminals that encode it as ESC CR/LF.
  if(a=='\r' || a=='\n')return KEY_NEWLINE;if(a!='[')return 27;std::string csi;for(int i=0;i<32;++i){unsigned char b=0;
  if(::read(STDIN_FILENO,&b,1)!=1)return KEY_IGNORED;csi.push_back(static_cast<char>(b));if(b<0x40 || b>0x7e)continue;
  if(csi=="A")return KEY_UP;if(csi=="B")return KEY_DOWN;if(csi=="C")return KEY_RIGHT;if(csi=="D")return KEY_LEFT;if(csi=="H"||csi=="1~")return KEY_HOME;
  if(csi=="F"||csi=="4~")return KEY_END;if(csi=="3~")return KEY_DELETE;if(csi=="5~")return KEY_PGUP;if(csi=="6~")return KEY_PGDN;
  if(csi=="13;2u")return KEY_NEWLINE;if(csi.rfind("<64;",0)==0 && csi.back()=='M')return KEY_SCROLL_UP;
  if(csi.rfind("<65;",0)==0 && csi.back()=='M')return KEY_SCROLL_DOWN;if(csi!="200~")return KEY_IGNORED;pasted_text_.clear();
  const std::string end="\x1b[201~";std::string pending;for(;;){unsigned char ch=0;if(::read(STDIN_FILENO,&ch,1)!=1)return KEY_IGNORED;
    pending.push_back(static_cast<char>(ch));if(pending==end)break;while(!pending.empty()&& end.compare(0,pending.size(),pending)!=0){
    if(pasted_text_.size()<1000000)pasted_text_.push_back(pending.front());pending.erase(0,1); }
  }
  std::string normalized;normalized.reserve(pasted_text_.size());for(size_t j=0;j<pasted_text_.size();++j){char ch=pasted_text_[j]; if(ch=='\r'){normalized+='\n';if(j+1<pasted_text_.size()&& pasted_text_[j+1]=='\n')++j;}
    else if(ch=='\n' || static_cast<unsigned char>(ch)>=32)normalized+=ch;
  }
  pasted_text_=std::move(normalized);return KEY_PASTE; } return KEY_IGNORED;
#endif
}
static size_t prev_utf8_boundary(const std::string& s,size_t pos){if(pos==0)return 0;size_t p=std::min(pos,s.size())-1; while(p>0 &&(static_cast<unsigned char>(s[p])&0xc0)==0x80)--p;return p;
}
static size_t next_utf8_boundary(const std::string& s,size_t pos){if(pos>=s.size())return s.size();size_t p=pos+1; while(p<s.size()&&(static_cast<unsigned char>(s[p])&0xc0)==0x80)++p;return p;
}
static size_t line_start_boundary(const std::string& s,size_t pos){ pos=std::min(pos,s.size());size_t p=s.rfind('\n',pos==0?0:pos-1);return p==std::string::npos?0:p+1;
}
static size_t line_end_boundary(const std::string& s,size_t pos){size_t p=s.find('\n',std::min(pos,s.size()));return p==std::string::npos?s.size():p; }
struct VisualBoundary{size_t byte=0,row=0,col=0;};static std::vector<VisualBoundary> visual_boundaries(const std::string& s,size_t width){
  width=std::max<size_t>(1,width);std::vector<VisualBoundary> out;size_t row=0,col=0;out.push_back({0,0,0});for(size_t i=0;i<s.size();){ if(s[i]=='\n'){++i;++row;col=0;out.push_back({i,row,col});continue;}
  size_t next=i+1;uint32_t cp=decode_utf8(s,i,next);size_t cw=static_cast<size_t>(std::max(0,codepoint_width(cp)));
  if(cw>0 && col>0 && col+cw>width){++row;col=0;out.push_back({i,row,col});}col+=cw;i=next;out.push_back({i,row,col}); } return out;
}
static bool move_cursor_vertical(const std::string& s,size_t& cursor,int delta,size_t width){auto bs=visual_boundaries(s,width);if(bs.empty())return false;
  size_t idx=0;for(size_t i=0;i<bs.size();++i)if(bs[i].byte<=cursor)idx=i;else break;const size_t row=bs[idx].row,col=bs[idx].col;
  if(delta<0 && row==0)return false;const size_t target=delta<0?row-1:row+1;bool found=false;size_t best=cursor,bestdist=static_cast<size_t>(-1);
  for(const auto& b:bs)if(b.row==target){size_t d=b.col>col?b.col-col:col-b.col;if(!found||d<bestdist){found=true;best=b.byte;bestdist=d;}} if(!found)return false;cursor=best;return true;
}
size_t current_composer_text_width()const{const size_t cols=std::max<size_t>(1,terminal_columns()),left_margin=2;
  const size_t composer_width=cols>left_margin*2?cols-left_margin*2:cols;return composer_width>4?composer_width-4:1;
}
static bool ansi_sequence_at(const std::string& s,size_t i,size_t& end){if(i >= s.size()|| static_cast<unsigned char>(s[i])!= 0x1b)return false;
  if(i + 1 >= s.size()){end = i + 1;return true;}if(s[i + 1]== '['){size_t j = i + 2;while(j < s.size()){ unsigned char c = static_cast<unsigned char>(s[j++]);if(c >= 0x40 && c <= 0x7e){end = j;return true;}
  }
  end = s.size();return true; } if(s[i + 1]== ']'){size_t j = i + 2;while(j < s.size()){if(s[j]== '\a'){end = j + 1;return true;}
    if(static_cast<unsigned char>(s[j])== 0x1b && j + 1 < s.size()&& s[j + 1]== '\\'){end = j + 2;return true; } ++j;
  }
  end = s.size();return true; } end = std::min(s.size(),i + 2);return true;
}
static uint32_t decode_utf8(const std::string& s,size_t i,size_t& next){const unsigned char c0 = static_cast<unsigned char>(s[i]);
  if(c0 < 0x80){next = i + 1;return c0;}auto cont=[&](size_t k){return k < s.size()&&(static_cast<unsigned char>(s[k])& 0xc0)== 0x80;};
  if((c0 & 0xe0)== 0xc0 && cont(i+1)){next=i+2;return((c0&0x1f)<<6)|(static_cast<unsigned char>(s[i+1])&0x3f); } if((c0 & 0xf0)== 0xe0 && cont(i+1)&& cont(i+2)){
  next=i+3;return((c0&0x0f)<<12)|((static_cast<unsigned char>(s[i+1])&0x3f)<<6)|(static_cast<unsigned char>(s[i+2])&0x3f); }
  if((c0 & 0xf8)== 0xf0 && cont(i+1)&& cont(i+2)&& cont(i+3)){next=i+4;return((c0&0x07)<<18)|((static_cast<unsigned char>(s[i+1])&0x3f)<<12)|
      ((static_cast<unsigned char>(s[i+2])&0x3f)<<6)|(static_cast<unsigned char>(s[i+3])&0x3f);
  }
  next=i+1;return 0xfffd;
}
static int codepoint_width(uint32_t cp){if(cp == 0)return 0;if(cp < 0x20 ||(cp >= 0x7f && cp < 0xa0))return 0;
  if((cp >= 0x0300 && cp <= 0x036f)||(cp >= 0x1ab0 && cp <= 0x1aff)||(cp >= 0x1dc0 && cp <= 0x1dff)||(cp >= 0x20d0 && cp <= 0x20ff)||
  (cp >= 0xfe00 && cp <= 0xfe0f)||(cp >= 0xfe20 && cp <= 0xfe2f)||(cp >= 0xe0100 && cp <= 0xe01ef))return 0;
  if((cp >= 0x1100 && cp <= 0x115f)|| cp == 0x2329 || cp == 0x232a ||(cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f)||
  (cp >= 0xac00 && cp <= 0xd7a3)||(cp >= 0xf900 && cp <= 0xfaff)||(cp >= 0xfe10 && cp <= 0xfe19)||(cp >= 0xfe30 && cp <= 0xfe6f)||
  (cp >= 0xff00 && cp <= 0xff60)||(cp >= 0xffe0 && cp <= 0xffe6)||(cp >= 0x1f300 && cp <= 0x1faff)||(cp >= 0x20000 && cp <= 0x3fffd))return 2;
  return 1;
}
static size_t display_width(const std::string& s){size_t cols=0;for(size_t i=0;i<s.size();){size_t end=0;if(ansi_sequence_at(s,i,end)){i=end;continue;}
  size_t next=i+1;const uint32_t cp=decode_utf8(s,i,next);cols += static_cast<size_t>(std::max(0,codepoint_width(cp)));i=next; } return cols;
}
static std::string truncate_display(const std::string& s,size_t width){std::string out;size_t cols=0;bool saw_ansi=false;for(size_t i=0;i<s.size();){
  size_t end=0;if(ansi_sequence_at(s,i,end)){out.append(s,i,end-i);saw_ansi=true;i=end;continue;}size_t next=i+1;const uint32_t cp=decode_utf8(s,i,next);
  const size_t cw=static_cast<size_t>(std::max(0,codepoint_width(cp)));if(cols+cw>width)break;out.append(s,i,next-i);cols+=cw;i=next; } if(saw_ansi && out.find("\x1b[")!=std::string::npos)out += "\x1b[0m";return out;
}
static std::string fit_display(const std::string& s,size_t width){std::string out=truncate_display(s,width);const size_t used=display_width(out); if(used<width)out.append(width-used,' ');return out;
}
static std::vector<std::string> wrap(const std::string& text,size_t width,size_t max_lines=9999){
  std::vector<std::string> out;if(width==0)return out;std::istringstream in(text);std::string line;while(std::getline(in,line)&& out.size()<max_lines){
  if(line.empty()){out.push_back("");continue;}while(display_width(line)>width && out.size()<max_lines){std::string part=truncate_display(line,width);
    if(part.empty())break;out.push_back(part);size_t consumed=0,cols=0;while(consumed<line.size()){size_t end=0;
    if(ansi_sequence_at(line,consumed,end)){consumed=end;continue;}size_t next=consumed+1;uint32_t cp=decode_utf8(line,consumed,next);
    size_t cw=static_cast<size_t>(std::max(0,codepoint_width(cp)));if(cols+cw>width)break;cols+=cw;consumed=next; } line.erase(0,consumed);
  }
  if(out.size()<max_lines && !line.empty())out.push_back(line); } return out;
}
static std::vector<std::string> wrap_words_display(const std::string& text,size_t width){std::vector<std::string> out;if(width == 0)return out;
  std::istringstream in(text);std::string word,line;auto flush_line =[&](){if(!line.empty()){out.push_back(line);line.clear();} };
  while(in >> word){if(display_width(word)> width){flush_line();auto pieces = wrap(word,width);out.insert(out.end(),pieces.begin(),pieces.end()); continue;
  }
  if(line.empty()){line = word; }else if(display_width(line)+ 1 + display_width(word)<= width){ line += " " + word;
  }else{
    flush_line();line = word;
  }
  }
  flush_line();if(out.empty())out.push_back("");return out;
}
static std::string markdown_inline(const std::string& source){std::string out;bool bold=false,italic=false,code=false;auto reset=[&](){out+="\x1b[0m";
  if(code)out+="\x1b[38;5;223;48;5;237m";else if(bold && italic)out+="\x1b[1;3m";else if(bold)out+="\x1b[1m";else if(italic)out+="\x1b[3m"; };
  for(size_t i=0;i<source.size();){if(source[i]=='\x1b' ||(static_cast<unsigned char>(source[i])<32 && source[i]!='\t')){++i;continue;}
  if(source[i]=='\\' && i+1<source.size()){out+=source[i+1];i+=2;continue;}if(source[i]=='`'){code=!code;reset();++i;continue;}
  if(!code && source.compare(i,2,"**")==0){bold=!bold;reset();i+=2;continue;}if(!code && source[i]=='*'){italic=!italic;reset();++i;continue;} if(!code && source[i]=='['){const size_t close=source.find("](",i+1);
    const size_t end=close==std::string::npos?std::string::npos:source.find(')',close+2);if(end!=std::string::npos && end-close<300){
    out+="\x1b[4;36m"+source.substr(i+1,close-i-1)+"\x1b[0m";reset();const std::string url=source.substr(close+2,end-close-2); if(!url.empty())out+=" \x1b[2m("+url+")\x1b[0m";reset();i=end+1;continue; }
  }
  out+=source[i++]; } if(bold||italic||code)out+="\x1b[0m";return out;
}
static std::vector<std::string> table_cells(const std::string& line){std::string s=trim(line);if(!s.empty()&& s.front()=='|')s.erase(0,1);
  if(!s.empty()&& s.back()=='|')s.pop_back();std::vector<std::string> cells;std::string cell;for(size_t i=0;i<s.size();++i){
  if(s[i]=='\\' && i+1<s.size()&& s[i+1]=='|'){cell+='|';++i;}else if(s[i]=='|'){cells.push_back(trim(cell));cell.clear();}else cell+=s[i]; } cells.push_back(trim(cell));return cells;
}
static bool table_separator(const std::string& line,size_t columns){const auto cells=table_cells(line);if(cells.size()!=columns)return false;
  for(const auto& cell:cells){size_t dashes=0;for(char c:cell){if(c=='-')++dashes;else if(c!=':' && c!=' ')return false; } if(dashes<3)return false; } return true;
}
static std::vector<std::string> markdown_lines(const std::string& text,size_t width){std::vector<std::string> source; std::istringstream input(text);std::string line;while(std::getline(input,line)){std::string safe;
  for(unsigned char ch:line)if(ch>=32 || ch=='\t')safe+=static_cast<char>(ch);source.push_back(std::move(safe)); }
  if(source.empty())source.push_back("");std::vector<std::string> out;auto append=[&](const std::string& styled,size_t indent=0){
  const size_t available=width>indent?width-indent:1;auto parts=wrap(styled,available);if(parts.empty()){out.push_back("");return;} for(auto& part:parts)out.push_back(std::string(indent,' ')+part); };
  bool fenced=false;for(size_t i=0;i<source.size();){std::string raw=source[i],t=trim(raw);if(t.rfind("```",0)==0 || t.rfind("~~~",0)==0){fenced=!fenced;
    if(fenced){if(!out.empty()&& !out.back().empty())out.push_back("");const std::string lang=trim(t.substr(3)); if(!lang.empty())append("\x1b[2m  "+lang+"\x1b[0m"); }else out.push_back(""); ++i;continue;
  }
  if(fenced){append("\x1b[2m│ \x1b[0m\x1b[38;5;223m"+raw+"\x1b[0m",2);++i;continue; } if(i+1<source.size()&& raw.find('|')!=std::string::npos){auto head=table_cells(raw);
    if(head.size()>=2 && head.size()<=12 && table_separator(source[i+1],head.size())){size_t end=i+2;std::vector<std::vector<std::string>> rows;
    while(end<source.size()&& source[end].find('|')!=std::string::npos){auto cells=table_cells(source[end]);if(cells.size()!=head.size())break; rows.push_back(std::move(cells));++end;
    }
    std::vector<size_t> widths(head.size(),1);for(size_t c=0;c<head.size();++c)widths[c]=std::max(widths[c],display_width(markdown_inline(head[c])));
    for(const auto& row:rows)for(size_t c=0;c<row.size();++c)widths[c]=std::max(widths[c],display_width(markdown_inline(row[c]))); size_t total=1;for(size_t w:widths)total+=w+3;
    const bool grid_possible=width>=1+7*head.size();// four text columns per cell
    if(grid_possible && total>width){const auto natural=widths;const size_t budget=width-1-3*head.size();size_t used=0;
      for(size_t c=0;c<widths.size();++c){widths[c]=std::min<size_t>(natural[c],4);used+=widths[c];}while(used<budget){
      size_t best=widths.size();double score=-1;for(size_t c=0;c<widths.size();++c)if(widths[c]<natural[c]){
        const double demand=static_cast<double>(natural[c])/static_cast<double>(widths[c]+1);if(demand>score){score=demand;best=c;}
      }
      if(best==widths.size())break;++widths[best];++used; }
    }
    if(grid_possible){auto border=[&](const std::string& left,const std::string& join,const std::string& right){std::string s=left;
      for(size_t c=0;c<widths.size();++c){for(size_t k=0;k<widths[c]+2;++k)s+="─";s+=c+1<widths.size()?join:right; } return "\x1b[2m"+s+"\x1b[0m"; };
      auto row_lines=[&](const std::vector<std::string>& cells,bool header){std::vector<std::vector<std::string>> pieces(cells.size());size_t height=1;
      for(size_t c=0;c<cells.size();++c){pieces[c]=wrap(markdown_inline(cells[c]),widths[c]);height=std::max(height,pieces[c].size()); }
      for(size_t r=0;r<height;++r){std::string s="\x1b[2m│\x1b[0m";for(size_t c=0;c<cells.size();++c){ std::string part=r<pieces[c].size()?pieces[c][r]:"";if(header)part="\x1b[1;36m"+part+"\x1b[0m";
        s+=" "+fit_display(part,widths[c])+" \x1b[2m│\x1b[0m"; } out.push_back(std::move(s));
      }
      };
      out.push_back(border("┌","┬","┐"));row_lines(head,true);out.push_back(border("├","┼","┤"));for(const auto& row:rows)row_lines(row,false); out.push_back(border("└","┴","┘"));
    }else{
      // Extremely narrow terminals cannot show four columns per cell.
      for(size_t r=0;r<rows.size();++r){if(r)out.push_back("");for(size_t c=0;c<head.size();++c) append("\x1b[1;36m"+head[c]+"\x1b[0m: "+markdown_inline(rows[r][c]),2);
      }
    }
    i=end;continue; }
  }
  if(t.empty()){out.push_back("");++i;continue;}size_t hashes=0;while(hashes<t.size()&& t[hashes]=='#')++hashes;
  if(hashes>0 && hashes<=6 && hashes<t.size()&& t[hashes]==' '){if(!out.empty()&& !out.back().empty())out.push_back(""); append("\x1b[1;38;5;81m"+markdown_inline(trim(t.substr(hashes+1)))+"\x1b[0m");
  }else if(t.rfind("> ",0)==0){
    append("\x1b[2m│ \x1b[0m"+markdown_inline(t.substr(2)),2);
  }else if(t.rfind("- ",0)==0 || t.rfind("* ",0)==0){
    append("\x1b[38;5;81m• \x1b[0m"+markdown_inline(t.substr(2)),2);
  }else{
    append(markdown_inline(raw));
  }
  ++i; } return out;
}
static std::string compact_args(const std::string& args){std::string s=args;for(char& c:s)if(c=='\n'||c=='\r'||c=='\t')c=' '; if(s.size()>160)s=s.substr(0,157)+"...";return s;
}
static std::string style_patch_line(const std::string& line){if(line.empty())return line;if(line[0]=='+')return "\x1b[32m"+line+"\x1b[0m";
  if(line[0]=='-')return "\x1b[31m"+line+"\x1b[0m";if(line.rfind("@@",0)==0)return "\x1b[36m"+line+"\x1b[0m";return line;
}
void add_cell(CellKind kind,const std::string& title,const std::string& body){ cells_.push_back({kind,title,body});if(cells_.size()>500)cells_.erase(cells_.begin(),cells_.begin()+100);scroll_offset_=0;
}
std::vector<std::string> cell_lines(const Cell& c,size_t width)const{std::vector<std::string> out;const size_t body_width = width > 4 ? width - 4 : width;
  auto body=wrap(c.body,body_width,c.kind==CellKind::Output?10:80);switch(c.kind){case CellKind::User:{if(body.empty())body.push_back("");
    out.push_back("");for(size_t i=0;i<body.size();++i)out.push_back((i==0?"\x1b[38;5;81;1m› \x1b[0m":"  ")+body[i]);break;
  }
  case CellKind::Assistant:{body=markdown_lines(c.body,body_width);out.push_back("");out.push_back("\x1b[38;5;81;1mCodex\x1b[0m"); for(auto& l:body)out.push_back("  "+l);out.push_back("");break;
  }
  case CellKind::Tool:{std::string head="\x1b[38;5;214m• \x1b[0m\x1b[38;5;223;1m"+c.title+"\x1b[0m"; if(!c.body.empty())head += " \x1b[2m· "+c.body+"\x1b[0m";out.push_back(head);break;
  }
  case CellKind::Output:{ for(size_t i=0;i<body.size();++i)out.push_back(std::string(i==0?"\x1b[2m  └ \x1b[0m":"\x1b[2m    \x1b[0m")+"\x1b[2m"+body[i]+"\x1b[0m");break;
  }
  case CellKind::Patch:{out.push_back("\x1b[38;5;214m• \x1b[0m\x1b[1m"+c.title+"\x1b[0m"); for(auto& l:body)out.push_back("\x1b[2m  └ \x1b[0m"+style_patch_line(l));break;
  }
  case CellKind::Error:{out.push_back("\x1b[31;1m✘ "+c.title+"\x1b[0m");for(auto& l:body)out.push_back("\x1b[2m  └ \x1b[0m"+l);break; } case CellKind::System:{for(auto& l:body)out.push_back("\x1b[2m"+l+"\x1b[0m");break;
  }
  case CellKind::SessionSummary:{const size_t card_w = std::min<size_t>(width > 4 ? width - 2 : width,64);std::string horiz;
    for(size_t i=0;i<(card_w>2?card_w-2:0);++i)horiz += "─";const std::string top = "┌" + horiz + "┐";const std::string bot = "└" + horiz + "┘";
    auto fit=[&](const std::string& x){const size_t inner=card_w>4?card_w-4:0;return "│ "+fit_display(x,inner)+" │";};
    out.push_back("\x1b[38;5;81m"+top+"\x1b[0m");out.push_back("\x1b[38;5;81m"+fit("›_  " + c.title)+"\x1b[0m");out.push_back(fit(""));
    out.push_back(fit("model:      " + model_));out.push_back(fit("directory:  " + root_));out.push_back("\x1b[38;5;81m"+bot+"\x1b[0m"); out.push_back("");break;
  }
  case CellKind::Notice:{if(body.empty())break;out.push_back(body.front());for(size_t i=1;i<body.size();++i)out.push_back("  "+body[i]); out.push_back("");break;
  }
  }
  return out;
}
std::vector<std::string> content_lines(size_t width,size_t budget)const{std::vector<std::string> lines;for(const auto& c:cells_){ auto chunk=cell_lines(c,width);lines.insert(lines.end(),chunk.begin(),chunk.end()); }
  const size_t max_offset=lines.size()>budget?lines.size()-budget:0;size_t end=lines.size()-std::min(scroll_offset_,max_offset);
  size_t begin=end>budget?end-budget:0;return std::vector<std::string>(lines.begin()+static_cast<long>(begin),lines.begin()+static_cast<long>(end));
}
std::string status_line()const{if(working_){ const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-working_started_).count();
  std::ostringstream ss;ss << "\x1b[38;5;214m• \x1b[0mWorking (" << elapsed << "s • esc to interrupt)";return ss.str(); } if(!status_.empty())return "\x1b[2m"+status_+"\x1b[0m";return{};
}
std::string footer_line()const{std::string cwd=root_;const char* home=std::getenv("HOME");if(home){std::string h(home); if(cwd.rfind(h,0)==0)cwd="~"+cwd.substr(h.size()); }
  std::vector<std::string> parts;if(footer_model_){std::string m="\x1b[38;5;223m"+model_;if(!effort_.empty())m+=" "+effort_; m+="\x1b[0m";parts.push_back(std::move(m)); }
  if(footer_thread_ && !thread_name_.empty())parts.push_back("\x1b[2m"+thread_name_+"\x1b[0m");
  if(footer_sandbox_)parts.push_back("\x1b[2m"+sandbox_+"\x1b[0m");if(footer_cwd_)parts.push_back("\x1b[38;5;151m"+cwd+"\x1b[0m");std::ostringstream ss;
  for(size_t i=0;i<parts.size();++i){if(i)ss<<"\x1b[2m · \x1b[0m";ss<<parts[i];}return ss.str();
}
std::vector<std::string> build_frame()const{const size_t cols=std::max<size_t>(1,terminal_columns()),rows=std::max<size_t>(1,terminal_rows());
  std::vector<std::string> f(rows,"");const bool has_status = working_ || !status_.empty();const size_t left_margin=2;
  const size_t composer_width = cols > left_margin * 2 ? cols - left_margin * 2 : cols;const size_t text_width = composer_width > 4 ? composer_width - 4 : 1;
  const auto composer_all_lines = composer_.empty()? std::vector<std::string>{std::string()}: wrap(composer_,text_width); const size_t wrapped_rows = std::max<size_t>(1,composer_all_lines.size());
  const size_t composer_h = std::min<size_t>(6,std::max<size_t>(3,wrapped_rows + 2));const size_t bottom_h =(has_status ? 1 : 0)+ composer_h + 2; const size_t panel_width=cols>left_margin*2?cols-left_margin*2:cols;
  const auto approval_lines=overlay_active_?wrap(overlay_body_,panel_width>6?panel_width-6:panel_width,4):std::vector<std::string>{};
  const size_t panel_wanted=overlay_active_?4+approval_lines.size()+overlay_options_.size():0;
  const size_t panel_h=rows>bottom_h+1?std::min(panel_wanted,rows-bottom_h-1):0;const size_t content_h=rows>bottom_h+panel_h?rows-bottom_h-panel_h:0;
  auto content=content_lines(cols>left_margin*2?cols-left_margin*2:cols,content_h);size_t y=0; for(auto& l:content)if(y<content_h)f[y++]=std::string(left_margin,' ')+l;const size_t base=rows-bottom_h;
  if(has_status)f[base]=std::string(left_margin,' ')+status_line();const size_t composer_row=base+(has_status?1:0);
  const size_t footer_row=composer_row+composer_h;const size_t hint_row=footer_row+1;const std::string placeholder="Use /help to list available commands";
  std::vector<std::string> composer_lines = composer_.empty()? std::vector<std::string>{placeholder}: wrap(composer_,text_width);
  if(composer_lines.empty())composer_lines.push_back("");auto vb=visual_boundaries(composer_,text_width);size_t cursor_visual_row=0;
  for(const auto& b:vb)if(b.byte<=std::min(composer_cursor_,composer_.size()))cursor_visual_row=b.row;else break;const size_t inner_rows=composer_h-2;
  const size_t composer_scroll=cursor_visual_row>=inner_rows?cursor_visual_row-inner_rows+1:0;for(size_t r=0;r<composer_h;++r){std::string plain;
  if(r>=1 && r<=inner_rows){const size_t li=composer_scroll+(r-1);if(li<composer_lines.size())plain=(li==0?"› ":"  ")+composer_lines[li]; }
  plain=fit_display(plain,composer_width);const std::string fg = composer_.empty()? "\x1b[38;5;245m" : "\x1b[97m"; f[composer_row+r]=std::string(left_margin,' ')+"\x1b[48;5;237m"+fg+plain+"\x1b[0m"; }
  f[footer_row]=std::string(left_margin+2,' ')+footer_line(); f[hint_row]=std::string(left_margin+2,' ')+"\x1b[2menter to send · ctrl+j newline · ↑↓ history/navigation\x1b[0m";{
  // Derive popup contents from the composer on every frame. Each command item can
  // occupy multiple terminal rows because its description may wrap. Layout and
  // scrolling therefore operate in visual rows, not in command-count units.
  auto matches = matching_slash_commands(composer_);if(!matches.empty()){const size_t selected = std::min(command_popup_selected_,matches.size()-1); const size_t popup_width = composer_width;
    const size_t prefix_width = 2;// "› " / "  "
    size_t command_col = 0;for(const auto* command : matches)command_col = std::max(command_col,display_width(command->name));
    // Keep enough room for useful description text even with unusually long names.
    if(popup_width > 24)command_col = std::min(command_col,popup_width / 3); const size_t gap_width = 2;const size_t desc_width = popup_width > prefix_width + command_col + gap_width
    ? popup_width - prefix_width - command_col - gap_width : 1; struct PopupItemLines{std::vector<std::string> lines;};std::vector<PopupItemLines> rendered;rendered.reserve(matches.size());
    for(size_t idx=0;idx<matches.size();++idx){const auto* command = matches[idx];const bool is_selected = idx == selected;
    auto desc_lines = wrap_words_display(command->description,desc_width);if(desc_lines.empty())desc_lines.push_back("");PopupItemLines item;
    item.lines.reserve(desc_lines.size());for(size_t j=0;j<desc_lines.size();++j){const std::string marker =(j==0 && is_selected)? "\x1b[96m› " : "  ";
      const std::string name_style = is_selected ? "\x1b[96m\x1b[1m" : "\x1b[97m";const std::string desc_style = is_selected ? "\x1b[96m" : "\x1b[2m";
      std::string line = marker;if(j==0)line += name_style + fit_display(command->name,command_col)+ "\x1b[0m";
      else line += std::string(command_col,' ');line += std::string(gap_width,' ')+ desc_style + desc_lines[j]+ "\x1b[0m"; item.lines.push_back(std::move(line));
    }
    rendered.push_back(std::move(item)); }
    // Reserve at most 12 visual rows above the composer. Pick the earliest item
    // that still allows the selected item to remain fully visible.
    const size_t max_popup_rows = std::min<size_t>(12,composer_row);size_t first = selected;size_t used = rendered[selected].lines.size();
    while(first > 0 && used + rendered[first-1].lines.size()<= max_popup_rows){--first;used += rendered[first].lines.size(); }
    size_t last = selected + 1;while(last < rendered.size()&& used + rendered[last].lines.size()<= max_popup_rows){used += rendered[last].lines.size(); ++last; }
    // If there is still space after filling downward, pull more items from above.
    while(first > 0 && used + rendered[first-1].lines.size()<= max_popup_rows){--first;used += rendered[first].lines.size(); }
    const size_t py = composer_row > used ? composer_row - used : 0;size_t row = py;for(size_t idx=first;idx<last && row<composer_row;++idx){
    for(const auto& line : rendered[idx].lines){if(row >= composer_row)break;f[row++]= std::string(left_margin,' ')+ truncate_display(line,popup_width); } }
  }
  }
  if(overlay_active_ && panel_h>0){const size_t oy=content_h;auto put=[&](size_t row,const std::string& txt){if(row>=panel_h)return;
    f[oy+row]=std::string(left_margin,' ')+"\x1b[48;5;236m"+ fit_display("  "+txt,panel_width)+"\x1b[0m";
  };
  for(size_t row=0;row<panel_h;++row)put(row,"");put(0,"\x1b[38;5;214;1m"+overlay_title_+"\x1b[0m"); for(size_t i=0;i<approval_lines.size()&& i+2<panel_h;++i)put(i+2,approval_lines[i]);
  const size_t options_y=panel_h>overlay_options_.size()+1?panel_h-overlay_options_.size()-1:1;
  const std::vector<std::string> defaults={"Yes, proceed","No, cancel"};const auto& opts=overlay_options_.empty()?defaults:overlay_options_; for(size_t i=0;i<opts.size()&& options_y+i<panel_h;++i){
    const std::string line=(static_cast<int>(i)==overlay_choice_?"\x1b[1m› ":"  ")+std::to_string(i+1)+". "+opts[i]+"\x1b[0m";put(options_y+i,line);
  }
  }
  return f;
}
void draw_diff(const std::vector<std::string>& frame){
  // A frame row must never be allowed to auto-wrap. Auto-wrap is destructive in a TUI:
  // a styled composer row that is even one column wider than the real terminal spills its
  // background into the footer/history row below. Clip every physical row to the *actual*
  // terminal width before diffing or drawing.
  const size_t cols=std::max<size_t>(1,terminal_columns());std::vector<std::string> clipped=frame;for(auto& line:clipped)line=truncate_display(line,cols);
  // Always reset SGR before erasing/redrawing a physical row.
  if(previous_frame_.size()!=clipped.size()){std::cout<<"\x1b[0m\x1b[2J";previous_frame_.assign(clipped.size(),"");}
  for(size_t i=0;i<clipped.size();++i)if(previous_frame_[i]!=clipped[i])std::cout<<"\x1b["<<(i+1)<<";1H\x1b[0m\x1b[2K"<<clipped[i]<<"\x1b[0m";
  previous_frame_=clipped;const size_t rows=clipped.size();const bool has_status = working_ || !status_.empty();const size_t left_margin=2;
  const size_t composer_width = cols > left_margin * 2 ? cols - left_margin * 2 : cols;const size_t text_width = composer_width > 4 ? composer_width - 4 : 1;
  const auto composer_all_lines = composer_.empty()? std::vector<std::string>{std::string()}: wrap(composer_,text_width); const size_t wrapped_rows = std::max<size_t>(1,composer_all_lines.size());
  const size_t composer_h = std::min<size_t>(6,std::max<size_t>(3,wrapped_rows + 2));const size_t bottom_h =(has_status ? 1 : 0)+ composer_h + 2;
  const size_t base=rows-bottom_h;const size_t composer_row=base+(has_status?1:0); auto vb=visual_boundaries(composer_,text_width);size_t cursor_row=0,cursor_col=0;
  for(const auto& b:vb)if(b.byte<=std::min(composer_cursor_,composer_.size())){cursor_row=b.row;cursor_col=b.col;}else break;
  const size_t inner_rows=composer_h-2;const size_t composer_scroll=cursor_row>=inner_rows?cursor_row-inner_rows+1:0;
  const size_t screen_row=composer_row+1+(cursor_row-composer_scroll);const size_t screen_col=left_margin+3+cursor_col; std::cout<<"\x1b["<<(screen_row+1)<<";"<<screen_col<<"H\x1b[?25h"<<std::flush;
}
bool active_=false,overlay_active_=false,working_=false,assistant_stream_active_=false;size_t assistant_stream_index_=0;
bool footer_model_=true,footer_cwd_=true,footer_thread_=true,footer_sandbox_=false;size_t command_popup_selected_=0;
int overlay_choice_=0;size_t scroll_offset_=0,composer_cursor_=0;std::chrono::steady_clock::time_point working_started_=std::chrono::steady_clock::now();
std::string root_,provider_,model_,effort_,sandbox_,thread_name_,status_,composer_,overlay_title_,overlay_body_;std::vector<std::string> overlay_options_;
std::vector<Cell> cells_;std::vector<std::string> previous_frame_; };
