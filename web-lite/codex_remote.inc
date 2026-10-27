// SSH owns authentication, encryption and terminal transport. No local model or tools run.
struct RemoteConnection {
  std::string host,project="default",port,team;
  bool collaborate=false,personal=false,project_set=false;
  bool check=false,login=false,help=false;
};
static bool remote_identifier(const std::string& value){
  if(value.empty()||value.size()>64)return false;
  for(char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
  return value.front()!='-'&&value.front()!='_';
}
static std::optional<RemoteConnection> parse_remote_connection(int argc,char** argv,std::string& error){
  RemoteConnection connection;
  if(argc==3&&(std::string(argv[2])=="--help"||std::string(argv[2])=="-h")){connection.help=true;return connection;}
  if(argc<3){error="Usage: codex-cpp connect USER@HOST [--project NAME] [--port PORT] [--check | --login]";return{};}
  connection.host=argv[2];
  if(connection.host.empty()||connection.host.size()>255||connection.host.front()=='-'||std::count(connection.host.begin(),connection.host.end(),'@')>1){error="Invalid SSH host; use USER@HOST or an SSH config alias";return{};}
  for(char c:connection.host)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||c=='@')){error="Invalid SSH host; use an SSH config alias for IPv6 or advanced options";return{};}
  if(connection.host.front()=='@'||connection.host.back()=='@'){error="Invalid SSH user or host";return{};}
  for(int i=3;i<argc;++i){const std::string option=argv[i];
    if(option=="--help"||option=="-h"){connection.help=true;continue;}
    if(option=="--collaborate"){connection.collaborate=true;continue;}
    if(option=="--personal"){connection.personal=true;continue;}
    if(option=="--team"&&i+1<argc){connection.team=argv[++i];if(!remote_identifier(connection.team)||connection.team.size()>40){error="Team project must be a valid identifier of at most 40 characters";return{};}connection.collaborate=true;continue;}
    if(option=="--check"){connection.check=true;continue;}
    if(option=="--login"){connection.login=true;continue;}
    if((option=="--project"||option=="--port")&&i+1<argc){const std::string value=argv[++i];
      if(option=="--project"){if(!remote_identifier(value)){error="Project must be 1..64 letters, digits, underscores or hyphens, starting with a letter or digit";return{};}connection.project=value;connection.project_set=true;}
      else{if(value.empty()||value.size()>5||value.find_first_not_of("0123456789")!=std::string::npos||std::stoi(value)<1||std::stoi(value)>65535){error="Port must be 1..65535";return{};}connection.port=value;}
      continue;
    }
    error="Unsupported connect option: "+option+"; configure providers on the server or use /provider";return{};
  }
  if((connection.personal&&connection.team.empty())||(connection.collaborate&&connection.project_set)||(connection.collaborate&&connection.login)||(connection.personal&&connection.check)){error="Use --team NAME [--personal] or --collaborate; private --project/--login cannot be combined with collaboration";return{};}
  if(connection.check&&connection.login){error="Choose either --check or --login";return{};}
  return connection;
}
static std::vector<std::string> remote_ssh_command(const RemoteConnection& connection){
  std::vector<std::string> command={"ssh",connection.check?"-T":"-t","-o","ForwardAgent=no","-o","ClearAllForwardings=yes","-o","PermitLocalCommand=no","-o","RemoteCommand=none"};
  if(!connection.port.empty()){command.push_back("-p");command.push_back(connection.port);}
  command.insert(command.end(),{"--",connection.host,"codex-cpp-remote"});
  if(connection.collaborate){if(connection.team.empty())command.push_back("--collaborate");else{command.push_back("--team");command.push_back(connection.team);}if(connection.personal)command.push_back("--personal");}
  else command.insert(command.end(),{"--project",connection.project});
  if(connection.check)command.push_back("--check");if(connection.login)command.push_back("--login");return command;
}
static int connect_remote(int argc,char** argv){
  std::string error;const auto connection=parse_remote_connection(argc,argv,error);
  if(!connection){std::cerr<<error<<'\n';return 2;}
  if(connection->help){std::cout<<"Usage: codex-cpp connect USER@HOST [--project NAME] [--port PORT] [--check | --login]\n"
    <<"       codex-cpp connect USER@HOST --team NAME [--personal]\n       codex-cpp connect USER@HOST --collaborate\n"
    <<"Runs the server's Agent in a private server-side project. SSH keys and host verification use your OpenSSH configuration.\n"
    <<"--check validates server dependencies without a terminal. --login signs in on the server.\n"
    <<"--collaborate opens the team console; --team NAME selects a shared project; add --personal for its independent personal Agent.\n"
    <<"Disconnect with tmux Ctrl+B, D; reconnect to the same user and project to continue.\n";return 0;}
  if(!connection->check&&!terminal_is_tty()){std::cerr<<"Remote sessions require a terminal. Use --check for a noninteractive readiness check.\n";return 2;}
  auto command=remote_ssh_command(*connection);std::vector<const char*> arguments;for(const auto& value:command)arguments.push_back(value.c_str());arguments.push_back(nullptr);
#if defined(_WIN32)
  const auto result=_spawnvp(_P_WAIT,"ssh",arguments.data());if(result!=-1)return static_cast<int>(result);
#else
  // Replace this process so SSH receives resize events, signals and terminal input directly.
  std::vector<char*> mutable_arguments;for(auto& value:command)mutable_arguments.push_back(value.data());mutable_arguments.push_back(nullptr);
  ::execvp("ssh",mutable_arguments.data());
#endif
  std::cerr<<"Could not start OpenSSH: "<<std::strerror(errno)<<". Install the OpenSSH client.\n";return 127;
}
