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
return 32; } struct SlashCommandDef{const char* name;const char* description; }; static const std::vector<SlashCommandDef>& slash_commands(){static const std::vector<SlashCommandDef> kCommands ={
  {"/model","choose what model and reasoning effort to use"},{"/login","sign in with ChatGPT device code"},
  {"/auth","show ChatGPT login status"},
  {"/permissions","choose what Codex is allowed to do"},
  {"/skills","list available project and user skills"},
  {"/review","review my current changes and find issues"},
  {"/rename","rename the current thread"},{"/clear(new)","start a new chat and reset context"},{"/archive","archive this session and exit"},
  {"/delete","permanently delete this session and exit"},{"/resume","resume a saved chat"},{"/fork","fork the current chat"},
  {"/continue","continue the unfinished turn"},{"/memory","view project memory or append a note"},
  {"/init","create an AGENTS.md file with instructions for Codex"},
  {"/compact","summarize conversation to prevent hitting the context limit"},{"/plan","switch to Plan mode"},{"/design","design and implement an interface (plan TASK for proposal only)"},
  {"/goal","set or view the goal for a long-running task"},
  {"/copy","copy last response (or: /copy code [N] for a code block)"},{"/mouse","toggle mouse capture; off allows selecting text with the mouse"},
  {"/diff","show git diff (including untracked files)"},
  {"/mention","mention a file"},{"/image","attach a PNG, JPEG, WebP or GIF to the next turn"},
  {"/status","show current session configuration and token usage"},
  {"/title","configure which items appear in the terminal title"},{"/statusline","configure which items appear in the status line"},
  {"/mcp","list connected local MCP stdio servers and tools"},{"/logout","log out of Codex"},
  {"/personality","choose a communication style for Codex"},
  // C++ port helpers. Hidden from the default popup but still accepted.
  {"/help","show available commands"},{"/sessions","list local saved sessions"},{"/history","show input history"}, {"/quit","exit Codex"},
};
return kCommands; } static const SlashCommandDef* find_slash_command(const std::string& name){for(const auto& command : slash_commands())if(name == command.name)return &command; return nullptr; }
static std::string slash_command_token(const std::string& input){if(input.empty()|| input.front()!= '/')return{};
const size_t end = input.find_first_of(" \t\r\n");return input.substr(0,end == std::string::npos ? input.size(): end); }
static std::vector<const SlashCommandDef*> matching_slash_commands(const std::string& input){std::vector<const SlashCommandDef*> out;
// Popup is valid only for a command token that starts at byte/column zero.
// Once whitespace/newline appears, the user is entering arguments, not filtering commands.
if(input.empty()|| input.front()!= '/' || input.find_first_of(" \t\r\n")!= std::string::npos)return out;
const std::string filter = slash_command_token(input);if(filter.empty())return out;for(const auto& command : slash_commands()){
  const std::string name = command.name;if(name == "/help" || name == "/sessions" || name == "/history" || name == "/quit")continue; if(name.rfind(filter,0)== 0 || (name=="/clear(new)" && std::string("/new").rfind(filter,0)==0))out.push_back(&command);
}
return out; }
static std::string opening_line(){static const char* lines[]={
"The source is strong with this one.","May your builds be green.","A small diff can make a big difference.","Every great program starts with a prompt.","Hello, world. What shall we build?","Less boilerplate, more possibilities.","Keep calm and commit carefully.","One good idea deserves a working prototype.","Make it work. Make it clear.","Your next idea is one prompt away.","Let curiosity drive the keyboard.","There is always room for one less bug.","Small steps. Useful software.","Today's TODO could be tomorrow's tool.","Turn coffee into code, carefully.","The best feature might be a deleted line.","A clean build is a lovely beginning.","Let's give that idea a command line.","Write a little. Learn a lot.","Good tools start with real problems.","Trust the tests. Read the diff.","Ship something small and useful.","Make the next step a good one.","A little less friction, a little more flow.","Build something you would use.","From 'what if' to 'it works'.","The terminal is yours.","Bring the idea. We'll find the edges.","Simplicity takes practice.","Let's make the boring parts easier.","先让它好用，再让它漂亮。","少一点重复，多一点创造。","今天的灵感，明天的工具。","从一个小问题开始。","代码不必多，刚好够用就好。","让想法跑起来。","愿今天的编译一次通过。","下一步，交给你的好奇心。","把麻烦变成一个命令。","写给未来的自己，也写得明白一点。"};static std::mt19937 rng{std::random_device{}()};return lines[std::uniform_int_distribution<size_t>(0,std::size(lines)-1)(rng)];}
class TuiRenderer{public: enum class CellKind{User,Assistant,Tool,Output,Patch,Error,System,SessionSummary,Notice};
struct Cell{CellKind kind;std::string title;std::string body;};TuiRenderer(const fs::path& root,std::string provider,std::string model,std::string sandbox)
  : root_(root.string()),provider_(std::move(provider)),model_(std::move(model)),sandbox_(std::move(sandbox)){
  std::cout << "\x1b[?1049h\x1b[?2004h\x1b[?1000h\x1b[?1003h\x1b[?1006h\x1b[?25l\x1b[2J\x1b[H" << std::flush;active_ = true; cells_.push_back({CellKind::SessionSummary,"Codex C++",""});
  cells_.push_back({CellKind::Notice,"",opening_line()});render();
}
~TuiRenderer(){if(active_)std::cout << "\x1b[?25h\x1b[?1006l\x1b[?1003l\x1b[?1000l\x1b[?2004l\x1b[?1049l" << std::flush; } TuiRenderer(const TuiRenderer&)= delete;TuiRenderer& operator=(const TuiRenderer&)= delete;
void add_user(const std::string& text){add_cell(CellKind::User,"",text);}void add_tool(const std::string& name,const std::string& args ={}){
  std::string label;if(name == "shell")label = "Ran shell command";else if(name == "read_file")label = "Read file";
  else if(name == "list_dir")label = "Explored directory";else if(name == "write_file")label = "Wrote file";
  else if(name == "apply_patch")label = "Applied patch";else label = name;
  // Show the one argument a person cares about (the command or path) instead of raw JSON.
  std::optional<std::string> key;if(name == "shell")key = json_string_field(args,"command");else if(name == "read_file" || name == "list_dir" || name == "write_file")key = json_string_field(args,"path");
  else if(name == "read_skill")key = json_string_field(args,"name");else if(name == "apply_patch")key = std::string();
  add_cell(CellKind::Tool,label,key ? compact_args(*key): compact_args(args));
}
void add_tool_output(const std::string& text){std::string shown = sanitize_text(text);if(shown.size()> 5000)shown = shown.substr(0,5000)+ "\n[output truncated]"; add_cell(CellKind::Output,"",shown);
}
void add_diff(const std::string& patch){add_cell(CellKind::Patch,"Edited files",patch);} void add_assistant(const std::string& text){add_cell(CellKind::Assistant,"",text);}
void set_status_details(const std::string& text){status_details_=text;}
void set_pinned_status(const std::string& text){pinned_text_=text;render_idle();}bool pinned_status()const{return !pinned_text_.empty();}
void set_bar_enabled(bool on){bar_enabled_=on;render_idle();}
void set_context_usage(size_t bytes,size_t limit){context_pct_=limit?static_cast<int>(std::min<size_t>(100,bytes*100/limit)):-1;}
void set_quota_brief(const std::string& text){quota_brief_=text;}
void set_tick(std::function<void()> callback){tick_cb_=std::move(callback);}
void add_error(const std::string& text){add_cell(CellKind::Error,"Error",text);}void add_system(const std::string& text){add_cell(CellKind::System,"",text);}
void reset_transcript(){pastes_.clear();paste_id_=0;pasted_text_.clear();steering_.clear();queued_.clear();composer_.clear();composer_cursor_=0;input_bytes_.clear();input_offset_=0;input_escape_.clear();input_utf8_.clear();paste_pending_.clear();input_paste_=false;active_pastes_.clear();cells_.clear();scroll_offset_=0; cells_.push_back({CellKind::SessionSummary,"Codex C++",""});
  cells_.push_back({CellKind::Notice,"",opening_line()});render();
}
void set_design(bool on){design_=on;render_idle();}
void set_model(std::string model,std::string effort ={}){model_=std::move(model);effort_=std::move(effort);render();}
void set_sandbox(std::string sandbox){sandbox_=std::move(sandbox);render();}void set_thread_name(std::string name){thread_name_=std::move(name);render();}
void set_statusline_items(const std::vector<std::string>& items){compact_footer_=false;footer_model_=footer_cwd_=footer_thread_=footer_sandbox_=false;for(const auto& item:items){
  if(item=="model")footer_model_=true;else if(item=="cwd"||item=="directory")footer_cwd_=true; else if(item=="thread"||item=="name")footer_thread_=true;else if(item=="sandbox"||item=="permissions")footer_sandbox_=true; }
  render();
}
int choose(const std::string& title,const std::vector<std::string>& options,size_t selected=0){if(options.empty())return -1;
  overlay_active_=true;overlay_title_=title;overlay_options_=options;overlay_choice_=static_cast<int>(std::min(selected,options.size()-1));render();
#if !defined(_WIN32)
  termios oldt{};bool raw_ok=tcgetattr(STDIN_FILENO,&oldt)==0;
  if(raw_ok){termios raw=oldt;raw.c_lflag&=static_cast<tcflag_t>(~(ICANON|ECHO));raw.c_iflag&=static_cast<tcflag_t>(~(IXON|ICRNL));tcsetattr(STDIN_FILENO,TCSANOW,&raw);}
#endif
  watch_resize(true);int result=-1;for(;;){int k=read_key();
#if !defined(_WIN32)
  if(tui_interrupt_requested){result=-1;break;}
#endif
  if(k==KEY_RESIZE){render();continue;}if(k==27){result=-1;break;} if(k==KEY_UP){overlay_choice_=(overlay_choice_+static_cast<int>(options.size())-1)%static_cast<int>(options.size());render();continue;}
  if(k==KEY_DOWN){overlay_choice_=(overlay_choice_+1)%static_cast<int>(options.size());render();continue;}
  if(k>='1' && k<='9'){int n=k-'1';if(n<static_cast<int>(options.size())){result=n;break;}}if(k=='\r'||k=='\n'){result=overlay_choice_;break;} }
#if !defined(_WIN32)
  if(raw_ok)tcsetattr(STDIN_FILENO,TCSANOW,&oldt);
#endif
  watch_resize(false);overlay_active_=false;overlay_options_.clear();overlay_body_.clear();render();return result;
}
bool approval(const std::string& text){overlay_body_=text;const int choice=choose("Would you like to run this command?",{"Yes, proceed","No, cancel"},0); overlay_body_.clear();return choice==0;
}
void render(const std::string& footer = ""){status_ = footer; if(!footer.empty()&&(footer.find("working")!= std::string::npos || footer.find("running")!= std::string::npos)){
  if(!working_){working_=true;working_started_=std::chrono::steady_clock::now();turn_bytes_=0;} }else if(footer.empty()){ working_=false; } draw_diff(build_frame());
}
void append_assistant_delta(const std::string& delta){if(delta.empty())return;if(!assistant_stream_active_){add_cell(CellKind::Assistant,"",""); assistant_stream_index_=cells_.size()-1;assistant_stream_active_=true; }
  cells_[assistant_stream_index_].body+=delta;turn_bytes_+=delta.size();draw_diff(build_frame());
}
void finish_assistant_stream(const std::string& text){if(assistant_stream_active_)cells_[assistant_stream_index_].body=text;
  else if(!text.empty())add_cell(CellKind::Assistant,"",text);assistant_stream_active_=false;working_=false;status_.clear();draw_diff(build_frame());
}
std::string fold_paste(const std::string& raw){std::string text;for(size_t i=0;i<raw.size();++i){if(raw[i]=='\r'){text+='\n';if(i+1<raw.size()&&raw[i+1]=='\n')++i;}else text+=raw[i];}const size_t lines=std::count(text.begin(),text.end(),'\n')+(!text.empty()&&text.back()!='\n');if(lines<=10)return text;const auto label="[Pasted text #"+std::to_string(++paste_id_)+" +"+std::to_string(lines)+" lines]";pastes_.push_back({label,text});return label;}
// Only recorded composer ranges are paste objects; identical typed labels remain literal text.
void replace_input(std::string& text,size_t pos,size_t count,const std::string& insert){
  active_pastes_.erase(std::remove_if(active_pastes_.begin(),active_pastes_.end(),[&](auto& p){
    if(pos<p.start+p.length && pos+count>p.start)return true;
    if(p.start>=pos+count)p.start=p.start-count+insert.size();return false;
  }),active_pastes_.end());text.replace(pos,count,insert);
}
void insert_paste(std::string& text,size_t& cursor,const std::string& raw){const auto index=pastes_.size();const auto folded=paste_editing_?raw:fold_paste(raw);const size_t pos=cursor;replace_input(text,pos,0,folded);cursor+=folded.size();if(pastes_.size()>index){active_pastes_.push_back({pos,folded.size(),index});std::sort(active_pastes_.begin(),active_pastes_.end(),[](const auto& a,const auto& b){return a.start<b.start;});}}
std::string expand_pastes(const std::string& text)const{std::string out;size_t pos=0;for(const auto& p:active_pastes_)if(p.start>=pos && p.start+p.length<=text.size() && text.compare(p.start,p.length,pastes_[p.index].first)==0){out+=text.substr(pos,p.start-pos);out+=pastes_[p.index].second;pos=p.start+p.length;}return out+text.substr(pos);}
size_t paste_boundary(const std::string& text,size_t cursor,bool forward)const{if(!paste_editing_)for(const auto& p:active_pastes_)if(forward?cursor>=p.start&&cursor<p.start+p.length:cursor>p.start&&cursor<=p.start+p.length)return forward?p.start+p.length:p.start;return forward?next_utf8_boundary(text,cursor):prev_utf8_boundary(text,cursor);}
size_t snap_paste_cursor(const std::string&,size_t cursor)const{if(!paste_editing_)for(const auto& p:active_pastes_)if(cursor>p.start&&cursor<p.start+p.length)return p.start+p.length;return cursor;}
size_t paste_hit(size_t row,size_t col)const{for(const auto& b:paste_buttons_)if(b.row==row&&col>=b.col&&col<b.col+b.width)return b.index;return size_t(-1);}
void edit_paste(size_t index){if(index>=pastes_.size()||paste_editing_)return;const auto saved=composer_,status=status_;const auto ranges=active_pastes_;const size_t cursor=composer_cursor_;const bool working=working_;const auto old_label=pastes_[index].first;composer_=pastes_[index].second;composer_cursor_=0;active_pastes_.clear();paste_editing_=true;paste_saved_=false;std::vector<std::string> history;auto edited=read_line(history);const bool saved_edit=paste_saved_;paste_editing_=false;composer_=saved;composer_cursor_=cursor;active_pastes_=ranges;working_=working;status_=status;if(saved_edit){pastes_[index].second=edited;const size_t lines=std::count(edited.begin(),edited.end(),'\n')+(!edited.empty()&&edited.back()!='\n');auto label=old_label.substr(0,old_label.find(" +"))+" +"+std::to_string(lines)+" lines]";pastes_[index].first=label;for(auto& p:active_pastes_)if(p.index==index){const size_t start=p.start,old_size=p.length;if(start<composer_cursor_)composer_cursor_=composer_cursor_-old_size+label.size();composer_.replace(start,old_size,label);p.length=label.size();for(auto& other:active_pastes_)if(other.start>start)other.start=other.start-old_size+label.size();break;}}render_idle();}
std::vector<std::string> take_steering(){auto out=std::move(steering_);steering_.clear();return out;}
std::string take_queued(){if(queued_.empty())return{};auto out=queued_.front();queued_.erase(queued_.begin());return out;}
bool receive_busy_input(const char* keys,size_t count){input_bytes_.erase(0,input_offset_);input_offset_=0;input_bytes_.append(keys,count);for(;;){const int k=read_key(false);if(k==KEY_INCOMPLETE||k<0)break;if(k==27||k==3)return true;
  if(k==KEY_PASTE){insert_paste(composer_,composer_cursor_,pasted_text_);pasted_text_.clear();}
  else if(k=='\r'||k==9){auto message=trim(expand_pastes(composer_));if(!message.empty()){(k==9?queued_:steering_).push_back(message);composer_.clear();composer_cursor_=0;active_pastes_.clear();pastes_.clear();add_system(k==9?"Queued ("+std::to_string(queued_.size())+"): "+truncate_display(message,70):"Steering received; applies before the next model request.");}}
  else if(k==127||k==8){if(composer_cursor_){const auto prev=paste_boundary(composer_,composer_cursor_,false);replace_input(composer_,prev,composer_cursor_-prev,"");composer_cursor_=prev;}}
  else if(k==KEY_DELETE){if(composer_cursor_<composer_.size())replace_input(composer_,composer_cursor_,paste_boundary(composer_,composer_cursor_,true)-composer_cursor_,"");}
  else if(k==KEY_LEFT){composer_cursor_=paste_boundary(composer_,composer_cursor_,false);}
  else if(k==KEY_RIGHT){composer_cursor_=paste_boundary(composer_,composer_cursor_,true);}
  else if(k==KEY_HOME)composer_cursor_=snap_paste_cursor(composer_,line_start_boundary(composer_,composer_cursor_));
  else if(k==KEY_END)composer_cursor_=snap_paste_cursor(composer_,line_end_boundary(composer_,composer_cursor_));
  else if(k==KEY_CLICK||k==KEY_HOVER){hover_row_=click_row_;hover_col_=click_col_;if(k==KEY_CLICK){const auto index=paste_hit(click_row_,click_col_);if(index!=size_t(-1))edit_paste(index);}}
  else if(k=='\n'||k==KEY_NEWLINE || (k>=32&&k<127) || k==KEY_TEXT){const auto text=k==KEY_TEXT?key_text_:std::string(1,char(k==KEY_NEWLINE?'\n':k));replace_input(composer_,composer_cursor_,0,text);composer_cursor_+=text.size();}
}draw_diff(build_frame());return false;}
void stream_assistant(const std::string& text){finish_assistant_stream(text);}std::string read_line(std::vector<std::string>& history){
#if defined(_WIN32)
  std::cout << "\x1b[?25h" << std::flush;std::string line;std::getline(std::cin,line);std::cout << "\x1b[?25l" << std::flush;return line;
#else
  termios oldt{};if(tcgetattr(STDIN_FILENO,&oldt)!= 0){std::string line;std::getline(std::cin,line);return line;}
  termios raw=oldt;raw.c_lflag &= static_cast<tcflag_t>(~(ICANON|ECHO));raw.c_iflag &= static_cast<tcflag_t>(~(IXON|ICRNL));raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
  load_skill_choices();picker_closed_=false;tcsetattr(STDIN_FILENO,TCSANOW,&raw);std::string line=composer_;size_t cursor=composer_cursor_,hist=history.size();bool popup=false;
  command_popup_selected_=0;working_=false;watch_resize(true);tick_active_=true;render();for(;;){int k=read_key();
#if !defined(_WIN32)
  if(tui_interrupt_requested){line.clear();break;}
#endif
  if(k<0){line.clear();break;}
  if(k==KEY_SCROLL_UP||k==KEY_SCROLL_DOWN||k==KEY_PGUP||k==KEY_PGDN){auto matches=composer_commands(line);if(!matches.empty()){const size_t step=(k==KEY_PGUP||k==KEY_PGDN)?6:1;const bool up=k==KEY_SCROLL_UP||k==KEY_PGUP;command_popup_selected_=up?(command_popup_selected_>step?command_popup_selected_-step:0):std::min(matches.size()-1,command_popup_selected_+step);render();continue;}}
  if(k==KEY_HOVER){const auto old_paste=paste_hit(hover_row_,hover_col_);auto hit=[&](size_t row,size_t col){for(size_t i=0;i<copy_buttons_.size();++i){const auto& b=copy_buttons_[i];if(row==b.row && col>=b.col && col<b.col+3)return i;}return size_t(-1);};const auto old=hit(hover_row_,hover_col_);hover_row_=click_row_;hover_col_=click_col_;if(old!=hit(hover_row_,hover_col_) || old_paste!=paste_hit(hover_row_,hover_col_))render();continue;}if(k==KEY_CLICK){const auto index=paste_hit(click_row_,click_col_);if(index!=size_t(-1)){edit_paste(index);line=composer_;cursor=composer_cursor_;tick_active_=true;watch_resize(true);render();continue;}if(paste_editing_ || !composer_commands(line).empty())continue;for(const auto& button:copy_buttons_)if(click_row_==button.row && click_col_>=button.col && click_col_<button.col+3){const auto err=clipboard_copy(button.text);add_system(err.empty()?"Code copied.":err);break;}render();continue;}if(k==KEY_IGNORED)continue;if(k==KEY_RESIZE){render();continue;}if(k==KEY_TICK){if(tick_cb_)tick_cb_();render();continue;}if(k==KEY_PASTE){
    // Treat the whole bracketed paste as text. In particular, its
    // newlines must not submit the prompt or select a slash item.
    insert_paste(line,cursor,pasted_text_);pasted_text_.clear();popup=false; composer_=line;composer_cursor_=cursor;render();continue;
  }
  // Enter submits. Ctrl+J inserts a real newline. Enhanced terminals may
  // report Shift+Enter as KEY_NEWLINE as well.
  if((k>=32 && k<=255) || k==127 || k==8 || k==KEY_DELETE || k==KEY_TEXT)picker_closed_=false;
  if(paste_editing_ && k==19){paste_saved_=true;break;}if(paste_editing_ && k=='\r')k='\n';
  if(k=='\r'){if(line=="/skills"){line="$";cursor=1;command_popup_selected_=0;composer_=line;composer_cursor_=cursor;render();continue;}auto matches = composer_commands(line);if(!matches.empty()){
    command_popup_selected_ = std::min(command_popup_selected_,matches.size()-1);active_pastes_.clear();line = matches[command_popup_selected_]->name;if(line=="/skills"){line="$";cursor=1;command_popup_selected_=0;composer_=line;composer_cursor_=cursor;render();continue;}if(line.front()=='$'){line+=" ";cursor=line.size();composer_=line;composer_cursor_=cursor;render();continue;}cursor = line.size();
    // A selected bare slash command is accepted immediately, matching Codex's popup behavior.
    break; } if(!line.empty())break;else continue;
  }
  if(k=='\n' || k==KEY_NEWLINE){replace_input(line,cursor,0,"\n");++cursor;}else if(k==3){line.clear();break;}
  else if(k==27){if(!paste_editing_ && (!composer_commands(line).empty() || (!line.empty() && line.front()=='$' && !picker_closed_))){popup=false;picker_closed_=true;render();continue;}line.clear();break;}else if(k==127 || k==8){
    if(cursor>0){size_t prev=paste_boundary(line,cursor,false);replace_input(line,prev,cursor-prev,"");cursor=prev;}
  }
  else if(k==KEY_DELETE){if(cursor<line.size()){size_t next=paste_boundary(line,cursor,true);replace_input(line,cursor,next-cursor,"");} } else if(k==KEY_LEFT){if(cursor>0)cursor=paste_boundary(line,cursor,false);}
  else if(k==KEY_RIGHT){if(cursor<line.size())cursor=paste_boundary(line,cursor,true);}else if(k==KEY_HOME)cursor=line_start_boundary(line,cursor);
  else if(k==KEY_END)cursor=line_end_boundary(line,cursor);else if(k==KEY_UP){auto matches = composer_commands(line);if(!matches.empty()){
    if(command_popup_selected_ == 0)command_popup_selected_ = matches.size()-1;else --command_popup_selected_; }else{
    const size_t text_width=current_composer_text_width();if(!move_cursor_vertical(line,cursor,-1,text_width)&& !history.empty()){ if(hist>0)--hist;active_pastes_.clear();line=history[hist];cursor=line.size();
    }
    }
  }
  else if(k==KEY_DOWN){auto matches = composer_commands(line);if(!matches.empty()){ command_popup_selected_ =(command_popup_selected_ + 1)% matches.size(); }else{
    const size_t text_width=current_composer_text_width();if(!move_cursor_vertical(line,cursor,+1,text_width)&& !history.empty()){
      if(hist<history.size())++hist;active_pastes_.clear();line=hist<history.size()?history[hist]:"";cursor=line.size();
    }
    }
  }
  else if(k==KEY_PGUP||k==21){scroll_offset_ += 4;}else if(k==KEY_PGDN||k==4){scroll_offset_ = scroll_offset_>4?scroll_offset_-4:0;}
  else if(k==KEY_SCROLL_UP){++scroll_offset_;}else if(k==KEY_SCROLL_DOWN){if(scroll_offset_)--scroll_offset_;}else if((k>=32 && k<127) || k==KEY_TEXT){const auto text=k==KEY_TEXT?key_text_:std::string(1,char(k));replace_input(line,cursor,0,text);cursor+=text.size();}
  // Derive popup visibility from the current composer contents on every key.
  // It is never allowed to survive as stale UI state.
  auto matches = composer_commands(line);popup = !matches.empty();if(!popup)command_popup_selected_ = 0; else if(command_popup_selected_ >= matches.size())command_popup_selected_ = matches.size()-1;
  cursor=snap_paste_cursor(line,cursor);composer_=line;composer_cursor_=cursor;render(); } tcsetattr(STDIN_FILENO,TCSANOW,&oldt);tick_active_=false;watch_resize(false);const auto result=paste_editing_?line:expand_pastes(line);composer_.clear();composer_cursor_=0;active_pastes_.clear();if(!paste_editing_)pastes_.clear();render();return result;
#endif
}
// While the program blocks outside the composer (e.g. device login), stop the terminal echoing
// mouse/key reports as garbage and discard whatever arrived in the meantime.
void quiet_input(bool on){
#if !defined(_WIN32)
  if(on){if(tcgetattr(STDIN_FILENO,&quiet_saved_)==0){quiet_valid_=true;termios t=quiet_saved_;t.c_lflag&=static_cast<tcflag_t>(~(ECHO|ICANON));t.c_cc[VMIN]=1;t.c_cc[VTIME]=0;tcsetattr(STDIN_FILENO,TCSANOW,&t);} }
  else if(quiet_valid_){tcflush(STDIN_FILENO,TCIFLUSH);tcsetattr(STDIN_FILENO,TCSANOW,&quiet_saved_);quiet_valid_=false;}
#else
  (void)on;
#endif
}
void set_mouse(bool on){mouse_=on;if(!on)hover_row_=hover_col_=size_t(-1);if(active_)std::cout<<(on?"\x1b[?1000h\x1b[?1003h\x1b[?1006h":"\x1b[?1003l\x1b[?1000l\x1b[?1006l")<<std::flush;}bool mouse()const{return mouse_;}
void render_idle(){if(active_)draw_diff(build_frame());}
void prompt(){working_=false;status_.clear();render();} private:
void load_skill_choices(){skill_labels_.clear();skill_choices_.clear();std::istringstream in(skills_manifest(root_));std::string line;while(std::getline(in,line)){auto colon=line.find(':');if(colon!=std::string::npos)skill_labels_.push_back({"$"+line.substr(0,colon),"[Skill] "+trim(line.substr(colon+1))});}for(const auto& item:skill_labels_)skill_choices_.push_back({item.first.c_str(),item.second.c_str()});}
std::vector<const SlashCommandDef*> composer_commands(const std::string& input)const{if(paste_editing_ || working_ || picker_closed_)return{};if(input.empty()||input.front()!='$')return matching_slash_commands(input);std::vector<const SlashCommandDef*> out;if(input.find_first_of(" \t\n")!=std::string::npos)return out;for(const auto& skill:skill_choices_)if(std::string(skill.name).find(input)!=std::string::npos)out.push_back(&skill);return out;}
bool picker_closed_=false;std::vector<std::pair<std::string,std::string>> skill_labels_;std::vector<SlashCommandDef> skill_choices_;
static constexpr int KEY_LEFT=1001,KEY_RIGHT=1002,KEY_UP=1003,KEY_DOWN=1004,KEY_HOME=1005,KEY_END=1006,KEY_PGUP=1007,KEY_PGDN=1008,
      KEY_DELETE=1009,KEY_NEWLINE=1010,KEY_PASTE=1011,KEY_IGNORED=1012,KEY_SCROLL_UP=1013,KEY_SCROLL_DOWN=1014,KEY_RESIZE=1015,KEY_TICK=1016,KEY_CLICK=1017,KEY_HOVER=1018,KEY_INCOMPLETE=1019,KEY_TEXT=1020;
// While waiting for input, a resize interrupts the blocking read so the layout redraws at once.
// It is enabled only there: elsewhere an EINTR could surface as a spurious I/O failure.
static void watch_resize(bool on){
#if !defined(_WIN32)
  struct sigaction action{};action.sa_handler=on?+[](int){}:SIG_DFL;sigemptyset(&action.sa_mask);action.sa_flags=0;sigaction(SIGWINCH,&action,nullptr);
#else
  (void)on;
#endif
}
#if !defined(_WIN32)
termios quiet_saved_{};bool quiet_valid_=false;
#endif
bool tick_active_=false;std::function<void()> tick_cb_;
// Shared incremental decoder: partial CSI, UTF-8 and paste packets survive busy/idle transitions.
std::string input_bytes_,input_escape_,paste_pending_,pasted_text_,key_text_,input_utf8_;size_t input_offset_=0;bool input_paste_=false;
int input_byte(bool wait,int timeout=-1){if(input_offset_<input_bytes_.size())return static_cast<unsigned char>(input_bytes_[input_offset_++]);
#if defined(_WIN32)
  return wait?std::cin.get():KEY_INCOMPLETE;
#else
  struct pollfd fd{STDIN_FILENO,POLLIN,0};const int ready=::poll(&fd,1,wait?timeout:0);if(ready==0)return KEY_INCOMPLETE;if(ready<0)return errno==EINTR&&!tui_interrupt_requested?KEY_RESIZE:-1;unsigned char c=0;if(::read(STDIN_FILENO,&c,1)!=1)return errno==EINTR&&!tui_interrupt_requested?KEY_RESIZE:-1;return c;
#endif
}
int read_key(bool wait=true){for(;;){int c=input_byte(wait,input_escape_=="\x1b"?40:wait&&tick_active_&&tick_cb_?5000:-1);if(c==KEY_INCOMPLETE && input_escape_=="\x1b"){if(!wait)c=input_byte(true,40);if(c==KEY_INCOMPLETE){input_escape_.clear();return 27;}}
  if(c==KEY_INCOMPLETE)return wait?KEY_TICK:KEY_INCOMPLETE;if(c<0||c==KEY_RESIZE)return c;
  if(input_paste_){const std::string end="\x1b[201~";paste_pending_+=char(c);if(paste_pending_==end){input_paste_=false;paste_pending_.clear();std::string normalized;for(size_t i=0;i<pasted_text_.size();++i){const auto ch=static_cast<unsigned char>(pasted_text_[i]);if(ch=='\r'){normalized+='\n';if(i+1<pasted_text_.size()&&pasted_text_[i+1]=='\n')++i;}else if(ch=='\t'||ch=='\n'||ch>=32)normalized+=char(ch);}pasted_text_=std::move(normalized);return KEY_PASTE;}while(!paste_pending_.empty()&&end.compare(0,paste_pending_.size(),paste_pending_)!=0){if(pasted_text_.size()<1000000)pasted_text_+=paste_pending_.front();paste_pending_.erase(0,1);}continue;}
  if(!input_utf8_.empty()){if((c&0xc0)!=0x80){input_bytes_.insert(input_offset_,1,char(c));key_text_=std::move(input_utf8_);input_utf8_.clear();return KEY_TEXT;}input_utf8_+=char(c);const unsigned char first=input_utf8_.front();const size_t length=(first&0xe0)==0xc0?2:(first&0xf0)==0xe0?3:4;if(input_utf8_.size()<length)continue;key_text_=std::move(input_utf8_);input_utf8_.clear();return KEY_TEXT;}
  if(input_escape_.empty()){if(c==27){input_escape_=char(c);continue;}if(c>=128){input_utf8_=char(c);if((c&0xe0)==0xc0||(c&0xf0)==0xe0||(c&0xf8)==0xf0)continue;key_text_=std::move(input_utf8_);input_utf8_.clear();return KEY_TEXT;}return c;}
  input_escape_+=char(c);if(input_escape_.size()==2){if(c=='\r'||c=='\n'){input_escape_.clear();return KEY_NEWLINE;}if(c!='['&&c!='O'){input_escape_.clear();return 27;}continue;}
  if(c<0x40||c>0x7e){if(input_escape_.size()>34){input_escape_.clear();return KEY_IGNORED;}continue;}
  const auto csi=input_escape_.substr(2);input_escape_.clear();
  if(csi=="A")return KEY_UP;if(csi=="B")return KEY_DOWN;if(csi=="C")return KEY_RIGHT;if(csi=="D")return KEY_LEFT;if(csi=="H"||csi=="1~")return KEY_HOME;
  if(csi=="F"||csi=="4~")return KEY_END;if(csi=="3~")return KEY_DELETE;if(csi=="5~")return KEY_PGUP;if(csi=="6~")return KEY_PGDN;if(csi=="13;2u")return KEY_NEWLINE;
  int button=0,x=0,y=0;char action=0;if(std::sscanf(csi.c_str(),"<%d;%d;%d%c",&button,&x,&y,&action)==4 && action=='M' && x>0&&y>0){click_col_=size_t(x-1);click_row_=size_t(y-1);if(button==0)return KEY_CLICK;if(button>=32&&button<64)return KEY_HOVER;if(button==64)return KEY_SCROLL_UP;if(button==65)return KEY_SCROLL_DOWN;}
  if(csi!="200~")return KEY_IGNORED;input_paste_=true;pasted_text_.clear();paste_pending_.clear();
}}
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
// Hard wrap (soft=false) must stay byte-for-byte compatible with visual_boundaries(), which the
// composer cursor relies on. soft=true prefers breaking after spaces and carries active SGR styles
// onto continuation lines, for transcript prose.
static std::vector<std::string> wrap(const std::string& text,size_t width,size_t max_lines=9999,bool soft=false){
  std::vector<std::string> out;if(width==0)return out;std::istringstream in(text);std::string line;while(std::getline(in,line)&& out.size()<max_lines){
  if(line.empty()){out.push_back("");continue;}while(display_width(line)>width && out.size()<max_lines){
    size_t consumed=0,cols=0,space_end=0;bool prev_space=false,prev_ascii=false,cut_ascii=false;std::string active;
    std::string active_at_space;
    while(consumed<line.size()){size_t end=0;
    if(ansi_sequence_at(line,consumed,end)){const std::string seq=line.substr(consumed,end-consumed);if(seq=="\x1b[0m"||seq=="\x1b[m")active.clear();else if(seq.size()>2 && seq.back()=='m')active+=seq;consumed=end;continue;}
    size_t next=consumed+1;uint32_t cp=decode_utf8(line,consumed,next);size_t cw=static_cast<size_t>(std::max(0,codepoint_width(cp)));
    if(cols+cw>width){cut_ascii=cp<0x80 && cp!=' ';break;}cols+=cw;consumed=next;prev_space=(cp==' ');prev_ascii=cp<0x80 && cp!=' ';
    if(prev_space){space_end=consumed;active_at_space=active;} }
    size_t cut=consumed;std::string carry=active;
    if(soft && cut_ascii && prev_ascii && space_end>0 && space_end<cut){std::string head=line.substr(0,space_end);while(!head.empty()&&head.back()==' ')head.pop_back();
      if(display_width(head)>0){cut=space_end;carry=active_at_space;} }
    if(cut==0)break;std::string part=line.substr(0,cut);if(soft)while(!part.empty()&&part.back()==' ')part.pop_back();
    if(!carry.empty())part+="\x1b[0m";out.push_back(part);line.erase(0,cut);if(!carry.empty())line=carry+line;
    if(soft)while(line.size()>carry.size() && line[carry.size()]==' ')line.erase(carry.size(),1); }
  if(out.size()<max_lines && !line.empty())out.push_back(line); } return out;
}
// Paints a full-width background band. Embedded SGR resets would otherwise punch holes in it.
static std::string paint_bg(const std::string& s,size_t width,const std::string& bg){std::string body=fit_display(s,width),out=bg;size_t pos=0;
  for(;;){const size_t at=body.find("\x1b[0m",pos);if(at==std::string::npos){out.append(body,pos,std::string::npos);break;}out.append(body,pos,at+4-pos);out+=bg;pos=at+4;}
  return out+"\x1b[0m";
}
static std::string rule_line(size_t width){std::string s;for(size_t i=0;i<width;++i)s+="─";return s;}
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
static std::vector<std::string> markdown_lines(const std::string& text,size_t width,bool copy_hint=false,std::vector<std::pair<size_t,size_t>>* buttons=nullptr){std::vector<std::string> source; std::istringstream input(text);std::string line;while(std::getline(input,line)){std::string safe;
  for(unsigned char ch:line)if(ch>=32 || ch=='\t')safe+=static_cast<char>(ch);source.push_back(std::move(safe)); }
  if(source.empty())source.push_back("");std::vector<std::string> out;
  // marker is drawn on the first row only; wrapped rows get a hanging indent of indent+marker_w.
  auto append=[&](const std::string& styled,size_t indent=0,const std::string& marker={},size_t marker_w=0){
  const size_t lead=indent+marker_w;const size_t available=width>lead?width-lead:1;auto parts=wrap(styled,available,9999,true);
  if(parts.empty()){out.push_back(marker.empty()?"":std::string(indent,' ')+marker);return;}
  for(size_t k=0;k<parts.size();++k)out.push_back(k==0?std::string(indent,' ')+marker+parts[k]:std::string(lead,' ')+parts[k]); };
  const std::string code_bg="\x1b[48;5;236m";bool fenced=false;size_t block_no=0;
  for(size_t i=0;i<source.size();){std::string raw=source[i],t=trim(raw);if(t.rfind("```",0)==0 || t.rfind("~~~",0)==0){fenced=!fenced;
    if(fenced){if(!out.empty()&& !out.back().empty())out.push_back("");const std::string lang=trim(t.substr(3));++block_no;
      std::string label=lang;if(copy_hint && width>=4){if(buttons)buttons->emplace_back(out.size(),block_no-1);label=truncate_display(lang,width-4);label+=std::string(width-4-display_width(label),' ')+" \x1b[23m⧉ ";}
      out.push_back(paint_bg(label.empty()?"":"\x1b[2;3m "+label+"\x1b[0m",width,code_bg));out.push_back(paint_bg("",width,code_bg)); }else{out.push_back(paint_bg("",width,code_bg));if(i+1<source.size()&& !trim(source[i+1]).empty())out.push_back("");} ++i;continue;
  }
  if(fenced){std::string code;for(char ch:raw){if(ch=='\t')code+="    ";else code+=ch;}
    auto parts=wrap(code,width>2?width-2:1);if(parts.empty())parts.push_back("");
    for(const auto& part:parts)out.push_back(paint_bg("\x1b[38;5;223m "+part,width,code_bg));++i;continue; } if(i+1<source.size()&& raw.find('|')!=std::string::npos){auto head=table_cells(raw);
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
      for(size_t c=0;c<cells.size();++c){pieces[c]=wrap(markdown_inline(cells[c]),widths[c],9999,true);height=std::max(height,pieces[c].size()); }
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
  const size_t lead_spaces=raw.find_first_not_of(' ');const size_t nest=std::min<size_t>(lead_spaces==std::string::npos?0:lead_spaces/2,4);
  size_t digits=0;while(digits<t.size()&& digits<4 && std::isdigit(static_cast<unsigned char>(t[digits])))++digits;
  bool rule=t.size()>=3 && (t[0]=='-'||t[0]=='*'||t[0]=='_');if(rule)for(char ch:t)if(ch!=t[0]&&ch!=' '){rule=false;break;}
  if(rule){size_t marks=0;for(char ch:t)if(ch==t[0])++marks;rule=marks>=3;}
  if(hashes>0 && hashes<=6 && hashes<t.size()&& t[hashes]==' '){if(!out.empty()&& !out.back().empty())out.push_back("");
    const char* style=hashes==1?"\x1b[1;4;38;5;81m":hashes==2?"\x1b[1;38;5;81m":"\x1b[1;38;5;111m";append(style+markdown_inline(trim(t.substr(hashes+1)))+"\x1b[0m");
  }else if(t.rfind("> ",0)==0){
    append(markdown_inline(t.substr(2)),2,"\x1b[2m│ \x1b[0m",2);
  }else if(rule){
    out.push_back("\x1b[2m"+rule_line(std::min<size_t>(width,60))+"\x1b[0m");
  }else if(t.rfind("- ",0)==0 || t.rfind("* ",0)==0 || t.rfind("+ ",0)==0){
    static const char* const kBullets[]={"•","◦","▪","▫","·"};
    append(markdown_inline(t.substr(2)),2+nest*2,std::string("\x1b[38;5;81m")+kBullets[nest]+" \x1b[0m",2);
  }else if(digits>0 && digits+1<t.size()&&(t[digits]=='.'||t[digits]==')')&& t[digits+1]==' '){
    append(markdown_inline(t.substr(digits+2)),2+nest*2,"\x1b[38;5;81m"+t.substr(0,digits)+std::string(1,t[digits])+" \x1b[0m",digits+2);
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
static std::string home_relative(const std::string& path){const char* home=std::getenv("HOME");if(home && *home){const std::string h(home);
  if(path==h)return "~";if(path.size()>h.size()&& path.compare(0,h.size(),h)==0 && path[h.size()]=='/')return "~"+path.substr(h.size()); } return path;
}
// Keeps the tail of a long path (the informative part) and marks the cut with an ellipsis.
static std::string tail_ellipsis(const std::string& s,size_t width){if(display_width(s)<=width)return s;if(width<2)return truncate_display(s,width);
  std::vector<size_t> starts;for(size_t i=0;i<s.size();){starts.push_back(i);size_t next=i+1;decode_utf8(s,i,next);i=next;}size_t cols=0,from=s.size();
  for(size_t k=starts.size();k-->0;){size_t n2=starts[k]+1;const uint32_t cp=decode_utf8(s,starts[k],n2);
    const size_t cw=static_cast<size_t>(std::max(0,codepoint_width(cp)));if(cols+cw>width-1)break;cols+=cw;from=starts[k]; }
  return "…"+s.substr(from);
}
// Tool output is untrusted: strip escape sequences and control bytes that would corrupt the frame layout.
static std::string sanitize_text(const std::string& s){std::string out;out.reserve(s.size());for(size_t i=0;i<s.size();){size_t end=0;
  if(ansi_sequence_at(s,i,end)){i=end;continue;}const unsigned char ch=static_cast<unsigned char>(s[i]);
  if(ch=='\t')out+="    ";else if(ch=='\n'||ch>=32)out+=static_cast<char>(ch);++i; } return out;
}
std::vector<std::string> cell_lines(const Cell& c,size_t width,std::vector<std::pair<size_t,size_t>>* buttons=nullptr)const{std::vector<std::string> out;const size_t body_width = width > 4 ? width - 4 : width;
  const std::string accent=design_?"\x1b[38;5;183m":"\x1b[38;5;81m";
  auto body=wrap(c.body,body_width,c.kind==CellKind::Output?200:80,c.kind!=CellKind::Output && c.kind!=CellKind::Patch);switch(c.kind){case CellKind::User:{if(body.empty())body.push_back("");
    const std::string band="\x1b[48;5;236m";out.push_back("");
    for(size_t i=0;i<body.size();++i)out.push_back(paint_bg((i==0?"  \x1b[38;5;81;1m› \x1b[0m\x1b[97m":"    \x1b[97m")+body[i]+"\x1b[0m",width,band));
    out.push_back("");break;
  }
  case CellKind::Assistant:{body=markdown_lines(c.body,body_width,true,buttons);if(buttons)for(auto& button:*buttons)button.first+=2;out.push_back("");out.push_back("\x1b[38;5;81;1m• Codex\x1b[0m"); for(auto& l:body)out.push_back("  "+l);out.push_back("");break;
  }
  case CellKind::Tool:{const std::string title="\x1b[38;5;214m• \x1b[0m\x1b[38;5;223;1m"+c.title+"\x1b[0m";std::string head=title;
    if(!c.body.empty()){const size_t used=display_width(c.title)+4;const size_t room=width>used?width-used:0;std::string detail=c.body;
      if(display_width(detail)>room)detail=room>1?truncate_display(detail,room-1)+"…":std::string();
      if(!detail.empty())head+="  \x1b[2m"+detail+"\x1b[0m"; }
    out.push_back(head);break;
  }
  case CellKind::Output:{const size_t limit=8;const size_t shown=body.size()>limit+1?limit:body.size();
    for(size_t i=0;i<shown;++i)out.push_back(std::string(i==0?"\x1b[2m  └ \x1b[0m":"\x1b[2m    \x1b[0m")+"\x1b[2m"+body[i]+"\x1b[0m");
    if(shown<body.size())out.push_back("\x1b[2;3m    … +"+std::to_string(body.size()-shown)+" more lines\x1b[0m");break;
  }
  case CellKind::Patch:{size_t added=0,removed=0;std::vector<std::pair<std::string,int>> rows;{std::istringstream in(sanitize_text(c.body));std::string l;bool hunk=false;
      while(std::getline(in,l)){int kind=0;// 0 context, 1 add, 2 remove, 3 hunk header, 4 file header, 5 meta
        if(l.rfind("@@",0)==0){hunk=true;kind=3;}else if(l.rfind("diff ",0)==0 || l.rfind("index ",0)==0 || l.rfind("new file",0)==0 || l.rfind("deleted file",0)==0){hunk=l.rfind("diff ",0)==0?false:hunk;kind=5;}
        else if(!hunk &&(l.rfind("--- ",0)==0 || l.rfind("+++ ",0)==0))kind=4;else if(hunk && !l.empty()&& l[0]=='+'){kind=1;++added;}else if(hunk && !l.empty()&& l[0]=='-'){kind=2;++removed;}
        if(l.empty())rows.emplace_back("",kind);else for(auto& piece:wrap(l,body_width))rows.emplace_back(piece,kind); } }
    out.push_back("\x1b[38;5;214m• \x1b[0m\x1b[1m"+c.title+"\x1b[0m  \x1b[32m+"+std::to_string(added)+"\x1b[0m \x1b[31m-"+std::to_string(removed)+"\x1b[0m");
    const size_t limit=60;const size_t shown=std::min(rows.size(),limit);const size_t band_w=width>2?width-2:width;
    for(size_t i=0;i<shown;++i){const auto& r=rows[i];std::string line;
      switch(r.second){case 1:line=paint_bg("\x1b[38;5;120m"+r.first,band_w,"\x1b[48;5;22m");break;case 2:line=paint_bg("\x1b[38;5;217m"+r.first,band_w,"\x1b[48;5;52m");break;
      case 3:line="\x1b[36m"+r.first+"\x1b[0m";break;case 4:line="\x1b[1m"+r.first+"\x1b[0m";break;case 5:line="\x1b[2m"+r.first+"\x1b[0m";break;default:line="\x1b[2m"+r.first+"\x1b[0m";}
      out.push_back("  "+line); }
    if(shown<rows.size())out.push_back("\x1b[2;3m  … +"+std::to_string(rows.size()-shown)+" more lines\x1b[0m");break;
  }
  case CellKind::Error:{out.push_back("\x1b[31;1m✘ "+c.title+"\x1b[0m");for(auto& l:body)out.push_back("\x1b[2m  └ \x1b[0m\x1b[38;5;217m"+l+"\x1b[0m");break; }
  case CellKind::System:{for(auto& l:body)out.push_back("\x1b[2m"+l+"\x1b[0m");break;
  }
  case CellKind::SessionSummary:{out.push_back("");out.push_back("\x1b[38;5;75m>_ \x1b[97;1m"+c.title+"\x1b[0m");out.push_back("   \x1b[2m"+truncate_display(home_relative(root_),width>3?width-3:width)+"\x1b[0m");out.push_back("");break;}
  case CellKind::Notice:{if(body.empty())break;out.push_back("\x1b[38;5;75m"+body.front()+"\x1b[0m");for(size_t i=1;i<body.size();++i)out.push_back("\x1b[2m  "+body[i]+"\x1b[0m"); out.push_back("");break;
  }
  }
  return out;
}
std::vector<std::string> content_lines(size_t cols,size_t margin,size_t budget)const{std::vector<std::string> lines;const size_t width=cols>margin*2?cols-margin*2:cols;
  copy_buttons_.clear();for(const auto& c:cells_){ if(c.kind==CellKind::User){auto chunk=cell_lines(c,cols);lines.insert(lines.end(),chunk.begin(),chunk.end());continue;}
    std::vector<std::pair<size_t,size_t>> buttons;auto chunk=cell_lines(c,width,&buttons);if(!buttons.empty()){const auto blocks=extract_code_blocks(c.body);for(const auto& button:buttons)if(button.second<blocks.size())copy_buttons_.push_back({lines.size()+button.first,margin+2+(width>4?width-4:width)-3,blocks[button.second].text});}for(auto& l:chunk)l=std::string(margin,' ')+l;lines.insert(lines.end(),chunk.begin(),chunk.end()); }
  const size_t max_offset=lines.size()>budget?lines.size()-budget:0;if(scroll_offset_>max_offset)scroll_offset_=max_offset;size_t end=lines.size()-scroll_offset_;
  size_t begin=end>budget?end-budget:0;copy_buttons_.erase(std::remove_if(copy_buttons_.begin(),copy_buttons_.end(),[&](const CopyButton& b){return b.row<begin || b.row>=end;}),copy_buttons_.end());for(auto& button:copy_buttons_)button.row-=begin;return std::vector<std::string>(lines.begin()+static_cast<long>(begin),lines.begin()+static_cast<long>(end));
}
std::string status_line()const{if(working_){ const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-working_started_).count();
  static const char* const kSpinner[]={"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};const long secs=static_cast<long>(elapsed/1000);
  std::string label=trim(status_);while(!label.empty()&& label.back()=='.')label.pop_back();if(label.empty())label="working";label[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
  std::ostringstream ss;ss << "\x1b[38;5;214m" << kSpinner[(elapsed/80)%10] << " \x1b[0m\x1b[1m" << label << "\x1b[0m\x1b[2m  ";
  if(secs>=60)ss << secs/60 << "m " << (secs%60<10?"0":"") << secs%60 << "s";else ss << secs << "s";
  if(turn_bytes_>=40){const size_t tk=turn_bytes_/4;if(tk>=1000)ss << " · ~" << tk/1000 << "." << (tk%1000)/100 << "k tokens";else ss << " · ~" << tk << " tokens";}ss << " · esc to interrupt\x1b[0m";return ss.str(); } if(!status_.empty())return "\x1b[2m"+status_+"\x1b[0m";return{};
}
std::string footer_line()const{const std::string cwd=home_relative(root_);
  std::vector<std::string> parts;if(footer_model_){std::string m="\x1b[38;5;223m"+model_;if(!effort_.empty())m+=" "+effort_; m+="\x1b[0m";parts.push_back(std::move(m)); }
  if(footer_thread_ && !thread_name_.empty())parts.push_back("\x1b[2m"+thread_name_+"\x1b[0m");
  if(footer_sandbox_)parts.push_back((sandbox_=="danger-full-access"?"\x1b[31;1m":"\x1b[2m")+sandbox_+"\x1b[0m");if(footer_cwd_)parts.push_back("\x1b[38;5;151m"+cwd+"\x1b[0m");std::ostringstream ss;
  for(size_t i=0;i<parts.size();++i){if(i)ss<<"\x1b[2m · \x1b[0m";ss<<parts[i];}return ss.str();
}
std::string bar_line(size_t width)const{
  const std::string dim="\x1b[2m",rst="\x1b[0m";std::vector<std::string> parts;
  parts.push_back("\x1b[38;5;223;1m"+model_+rst+(effort_.empty()?"":dim+" "+effort_+rst));
  parts.push_back((sandbox_=="danger-full-access"?"\x1b[31;1m":"\x1b[38;5;151m")+sandbox_+rst);
  if(context_pct_>=0){const int filled=(context_pct_*6+50)/100;std::string meter;for(int i=0;i<6;++i)meter+=i<filled?"▰":"▱";
    const char* color=context_pct_>=90?"\x1b[31m":context_pct_>=70?"\x1b[38;5;214m":"\x1b[38;5;114m";parts.push_back(dim+"ctx "+rst+color+meter+" "+std::to_string(context_pct_)+"%"+rst); }
  if(!quota_brief_.empty())parts.push_back(dim+"left "+rst+quota_brief_);
  if(!thread_name_.empty())parts.push_back(dim+thread_name_+rst);
  std::string out;size_t used=0;for(size_t i=0;i<parts.size();++i){const size_t sep=i?3:0;const size_t w=display_width(parts[i]);if(used+sep+w>width && i>0)break;
    if(i)out+=dim+" │ "+rst;out+=parts[i];used+=sep+w;}
  return out;
}
// Floating bar (1 row) plus optional pinned status rows; hidden on short terminals.
std::vector<std::string> top_rows(size_t cols,size_t rows)const{std::vector<std::string> out;if((!bar_enabled_ && pinned_text_.empty())||rows<15)return out;
  const size_t margin=2,width=cols>margin*2?cols-margin*2:cols;const char* bg="\x1b[48;5;235m";
  if(bar_enabled_)out.push_back(std::string(margin,' ')+paint_bg(" "+bar_line(width>1?width-1:width),width,bg));
  if(!pinned_text_.empty()&&rows>=30){for(const auto& l:wrap(pinned_text_,width>4?width-4:width,6))out.push_back(std::string(margin,' ')+paint_bg("  \x1b[2m"+l+"\x1b[0m",width,bg)); }
  return out;
}
std::string hint_text()const{
  if(paste_editing_)return "\x1b[2mEditing pasted text · enter newline · ctrl+s save · esc cancel\x1b[0m";
  if(working_)return "\x1b[2menter steer · tab queue · ctrl+j newline · esc interrupt\x1b[0m";
  if(overlay_active_)return "\x1b[2m↑↓ select · enter confirm · esc cancel · 1-9 quick pick\x1b[0m";
  if(scroll_offset_>0)return "\x1b[38;5;214m↑ scrolled back\x1b[0m\x1b[2m · ctrl+d / fn+↓ / scroll down to return to the latest output\x1b[0m";
  if(!composer_.empty() && composer_.front()=='$' && !picker_closed_ && composer_.find_first_of(" \t\n")==std::string::npos)return composer_commands(composer_).empty()?"\x1b[2mNo matching skills · esc close\x1b[0m":"\x1b[2m↑↓ choose · enter insert · esc close\x1b[0m";
  if(!composer_commands(composer_).empty())return "\x1b[2m↑↓ choose · enter run · keep typing to filter\x1b[0m";
  return "\x1b[2menter send · ctrl+j newline · / commands · ctrl+u/d scroll\x1b[0m";
}
std::vector<std::string> build_frame()const{const size_t cols=std::max<size_t>(1,terminal_columns()),rows=std::max<size_t>(1,terminal_rows());
  std::vector<std::string> f(rows,"");const bool has_status = working_ || !status_.empty();const size_t left_margin=2;
  const size_t composer_width = cols > left_margin * 2 ? cols - left_margin * 2 : cols;const size_t text_width = composer_width > 4 ? composer_width - 4 : 1;
  const auto composer_all_lines = composer_.empty()? std::vector<std::string>{std::string()}: wrap(composer_,text_width); const size_t wrapped_rows = std::max<size_t>(1,composer_all_lines.size());
  const size_t composer_h = std::min<size_t>(paste_editing_?std::max<size_t>(3,rows>6?rows-6:3):6,std::max<size_t>(3,wrapped_rows + 2));const size_t bottom_h =(has_status ? 1 : 0)+ composer_h + 2 + wrap(status_details_,text_width,std::min<size_t>(6,rows>composer_h+3?rows-composer_h-3:0)).size(); const size_t panel_width=cols>left_margin*2?cols-left_margin*2:cols;
  const auto approval_lines=overlay_active_?wrap(overlay_body_,panel_width>6?panel_width-6:panel_width,4):std::vector<std::string>{};
  const size_t panel_wanted=overlay_active_?4+approval_lines.size()+overlay_options_.size():0;
  const auto top=top_rows(cols,rows);const size_t top_h=top.size();const size_t avail=rows>top_h?rows-top_h:0;
  const size_t panel_h=avail>bottom_h+1?std::min(panel_wanted,avail-bottom_h-1):0;const size_t content_h=avail>bottom_h+panel_h?avail-bottom_h-panel_h:0;
  auto content=content_lines(cols,left_margin,content_h);for(auto& button:copy_buttons_)button.row+=top_h;size_t y=top_h; for(size_t i=0;i<top_h;++i)f[i]=top[i];for(auto& l:content)if(y<top_h+content_h)f[y++]=l;const size_t base=rows-bottom_h;
  if(mouse_ && !overlay_active_ && composer_commands(composer_).empty())for(const auto& button:copy_buttons_)if(hover_row_==button.row && hover_col_>=button.col && hover_col_<button.col+3){
    auto& row=f[button.row];const auto icon=row.find("⧉");if(icon!=std::string::npos)row.replace(icon,std::string("⧉").size(),"\x1b[22;1;38;5;81;48;5;240m⧉\x1b[0m\x1b[48;5;236m");break;
  }
  if(has_status)f[base]=std::string(left_margin,' ')+status_line();const size_t composer_row=base+(has_status?1:0);
  const auto details=wrap(status_details_,text_width,std::min<size_t>(6,rows>composer_h+3?rows-composer_h-3:0));const size_t details_row=composer_row+composer_h;
  for(size_t i=0;i<details.size();++i)f[details_row+i]=std::string(left_margin+2,' ')+"\x1b[2m"+details[i]+"\x1b[0m";
  const size_t footer_row=details_row+details.size();const size_t hint_row=footer_row+1;const std::string placeholder=design_?"Describe the interface you want to design":"Ask Codex to do anything";
  std::vector<std::string> composer_lines = composer_.empty()? std::vector<std::string>{placeholder}: wrap(composer_,text_width);
  if(composer_lines.empty())composer_lines.push_back("");auto vb=visual_boundaries(composer_,text_width);size_t cursor_visual_row=0;
  for(const auto& b:vb)if(b.byte<=std::min(composer_cursor_,composer_.size()))cursor_visual_row=b.row;else break;const size_t inner_rows=composer_h-2;
  const size_t composer_scroll=cursor_visual_row>=inner_rows?cursor_visual_row-inner_rows+1:0;paste_buttons_.clear();if(!paste_editing_)for(const auto& p:active_pastes_){for(const auto& b:vb)if(b.byte>=p.start&&b.byte<p.start+p.length&&b.row>=composer_scroll&&b.row<composer_scroll+inner_rows){const size_t row=composer_row+1+b.row-composer_scroll,col=left_margin+2+b.col;if(!paste_buttons_.empty()&&paste_buttons_.back().index==p.index&&paste_buttons_.back().row==row&&paste_buttons_.back().col+paste_buttons_.back().width==col)++paste_buttons_.back().width;else paste_buttons_.push_back({row,col,1,p.index});}}
for(size_t r=0;r<composer_h;++r){std::string row;
  if(r>=1 && r<=inner_rows){const size_t li=composer_scroll+(r-1);const std::string fg = composer_.empty()? "\x1b[38;5;245m" : "\x1b[97m";
    if(li<composer_lines.size()){auto text=composer_lines[li];if(!paste_editing_)for(const auto& b:paste_buttons_)if(b.row==composer_row+r && paste_hit(hover_row_,hover_col_)==b.index){const size_t col=b.col-left_margin-2;size_t start=text.size(),end=text.size();for(const auto& boundary:visual_boundaries(text,text_width)){if(boundary.col==col)start=boundary.byte;if(boundary.col==col+b.width)end=boundary.byte;}if(start<end && end<=text.size())text.insert(end,"\x1b[0m"),text.insert(start,"\x1b[1;38;5;81;48;5;240m");}row=(li==0?"\x1b[38;5;81;1m› \x1b[22m"+fg:fg+"  ")+text;} }
  f[composer_row+r]=std::string(left_margin,' ')+paint_bg(row,composer_width,design_?"\x1b[48;5;60m":"\x1b[48;5;237m"); }
  f[footer_row]=std::string(left_margin+2,' ')+(design_?"\x1b[38;5;183mDesign\x1b[0m · ":"")+((bar_enabled_||!compact_footer_)?footer_line():"\x1b[2m"+sanitize_text(bar_line(composer_width>2?composer_width-2:composer_width))+"\x1b[0m"); f[hint_row]=std::string(left_margin+2,' ')+hint_text();
  if(context_pct_>=90 && !overlay_active_){const int left=std::max(0,100-context_pct_);const std::string note=std::string(context_pct_>=97?"\x1b[31m":"\x1b[38;5;214m")+std::to_string(left)+"% until auto-compact\x1b[0m";
    const size_t lw=left_margin+2+display_width(hint_text()),nw=display_width(note);if(cols>lw+nw+left_margin+2)f[hint_row]+=std::string(cols-lw-nw-left_margin,' ')+note; }{
  // Derive popup contents from the composer on every frame. Each command item can
  // occupy multiple terminal rows because its description may wrap. Layout and
  // scrolling therefore operate in visual rows, not in command-count units.
  auto matches = composer_commands(composer_);if(!matches.empty()){const size_t selected = std::min(command_popup_selected_,matches.size()-1); const size_t popup_width = composer_width;
    const size_t prefix_width = 2;// "› " / "  "
    size_t command_col = 0;for(const auto* command : matches)command_col = std::max(command_col,display_width(command->name));
    // Keep enough room for useful description text even with unusually long names.
    if(popup_width > 24)command_col = std::min(command_col,popup_width / 3); const size_t gap_width = 2;const size_t desc_width = popup_width > prefix_width + command_col + gap_width
    ? popup_width - prefix_width - command_col - gap_width : 1; struct PopupItemLines{std::vector<std::string> lines;bool selected=false;};std::vector<PopupItemLines> rendered;rendered.reserve(matches.size());
    for(size_t idx=0;idx<matches.size();++idx){const auto* command = matches[idx];const bool is_selected = idx == selected;
    auto desc_lines = wrap_words_display(std::string(command->description),desc_width);if(!composer_.empty() && composer_.front()=='$')desc_lines={truncate_display(command->description,desc_width)};if(desc_lines.empty())desc_lines.push_back("");PopupItemLines item;item.selected=is_selected;
    item.lines.reserve(desc_lines.size());for(size_t j=0;j<desc_lines.size();++j){const bool skill_picker=!composer_.empty() && composer_.front()=='$';const std::string accent=skill_picker?"\x1b[97m":"\x1b[96m";const std::string marker =(j==0 && is_selected)? accent+"› " : "  ";
      const std::string name_style = is_selected ? accent+"\x1b[1m" : "\x1b[97m";const std::string desc_style = is_selected && !skill_picker ? accent : "\x1b[2m";
      std::string line = marker;if(j==0)line += name_style + fit_display(command->name,command_col)+ "\x1b[0m";
      else line += std::string(command_col,' ');line += std::string(gap_width,' ')+ desc_style + desc_lines[j]+ "\x1b[0m"; item.lines.push_back(std::move(line));
    }
    rendered.push_back(std::move(item)); }
    // Reserve at most 12 visual rows above the composer. Pick the earliest item
    // that still allows the selected item to remain fully visible.
    const size_t max_popup_rows = std::min<size_t>(12,composer_row>top_h?composer_row-top_h:0);size_t first = selected;size_t used = rendered[selected].lines.size();
    while(first > 0 && used + rendered[first-1].lines.size()<= max_popup_rows){--first;used += rendered[first].lines.size(); }
    size_t last = selected + 1;while(last < rendered.size()&& used + rendered[last].lines.size()<= max_popup_rows){used += rendered[last].lines.size(); ++last; }
    // If there is still space after filling downward, pull more items from above.
    while(first > 0 && used + rendered[first-1].lines.size()<= max_popup_rows){--first;used += rendered[first].lines.size(); }
    const size_t py = composer_row > used + top_h ? composer_row - used : top_h;size_t row = py;for(size_t idx=first;idx<last && row<composer_row;++idx){
    for(const auto& line : rendered[idx].lines){if(row >= composer_row)break;f[row++]= std::string(left_margin,' ')+ paint_bg(truncate_display(line,popup_width),popup_width,rendered[idx].selected?"\x1b[48;5;238m":"\x1b[48;5;236m"); } }
  }
  }
  if(overlay_active_ && panel_h>0){const size_t oy=top_h+content_h;auto put=[&](size_t row,const std::string& txt,const char* bg="\x1b[48;5;236m"){if(row>=panel_h)return;
    f[oy+row]=std::string(left_margin,' ')+paint_bg("  "+txt,panel_width,bg);
  };
  for(size_t row=0;row<panel_h;++row)put(row,"");put(0,"\x1b[38;5;214;1m"+overlay_title_+"\x1b[0m"); for(size_t i=0;i<approval_lines.size()&& i+2<panel_h;++i)put(i+2,"\x1b[38;5;223m"+approval_lines[i]+"\x1b[0m");
  const size_t options_y=panel_h>overlay_options_.size()+1?panel_h-overlay_options_.size()-1:1;
  const std::vector<std::string> defaults={"Yes, proceed","No, cancel"};const auto& opts=overlay_options_.empty()?defaults:overlay_options_; for(size_t i=0;i<opts.size()&& options_y+i<panel_h;++i){
    const bool sel=static_cast<int>(i)==overlay_choice_;const std::string line=(sel?"\x1b[38;5;81;1m› ":"  ")+std::to_string(i+1)+". "+opts[i]+"\x1b[0m";put(options_y+i,line,sel?"\x1b[48;5;239m":"\x1b[48;5;236m");
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
  const size_t composer_h = std::min<size_t>(paste_editing_?std::max<size_t>(3,rows>6?rows-6:3):6,std::max<size_t>(3,wrapped_rows + 2));const size_t bottom_h =(has_status ? 1 : 0)+ composer_h + 2 + wrap(status_details_,text_width,std::min<size_t>(6,rows>composer_h+3?rows-composer_h-3:0)).size();
  const size_t base=rows-bottom_h;const size_t composer_row=base+(has_status?1:0); auto vb=visual_boundaries(composer_,text_width);size_t cursor_row=0,cursor_col=0;
  for(const auto& b:vb)if(b.byte<=std::min(composer_cursor_,composer_.size())){cursor_row=b.row;cursor_col=b.col;}else break;
  const size_t inner_rows=composer_h-2;const size_t composer_scroll=cursor_row>=inner_rows?cursor_row-inner_rows+1:0;
  const size_t screen_row=composer_row+1+(cursor_row-composer_scroll);const size_t screen_col=left_margin+3+cursor_col; std::cout<<"\x1b["<<(screen_row+1)<<";"<<screen_col<<"H\x1b[?25h"<<std::flush;
}
struct PasteRange{size_t start,length,index;};size_t paste_id_=0;std::vector<std::pair<std::string,std::string>> pastes_;std::vector<PasteRange> active_pastes_;
std::vector<std::string> steering_,queued_;
bool active_=false,overlay_active_=false,working_=false,assistant_stream_active_=false;size_t assistant_stream_index_=0;
struct PasteButton{size_t row,col,width,index;};mutable std::vector<PasteButton> paste_buttons_;bool paste_editing_=false,paste_saved_=false;
struct CopyButton{size_t row,col;std::string text;};mutable std::vector<CopyButton> copy_buttons_;size_t click_row_=0,click_col_=0,hover_row_=size_t(-1),hover_col_=size_t(-1);
bool design_=false;bool mouse_=true;bool compact_footer_=true;bool bar_enabled_=false;int context_pct_=-1;std::string quota_brief_,pinned_text_;
bool footer_model_=true,footer_cwd_=true,footer_thread_=true,footer_sandbox_=false;size_t command_popup_selected_=0;
int overlay_choice_=0;mutable size_t scroll_offset_=0;size_t composer_cursor_=0;size_t turn_bytes_=0;std::chrono::steady_clock::time_point working_started_=std::chrono::steady_clock::now();
std::string root_,provider_,model_,effort_,sandbox_,thread_name_,status_,status_details_,composer_,overlay_title_,overlay_body_;std::vector<std::string> overlay_options_;
std::vector<Cell> cells_;std::vector<std::string> previous_frame_; };
