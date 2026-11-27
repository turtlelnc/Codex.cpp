// Conservative estimates, not a tokenizer. UTF-8 bytes are deliberately charged
// more than ASCII; inline image payloads have a separate fixed allowance.
static size_t estimate_tokens(const std::string& text){size_t ascii=0,other=0,images=0;for(size_t i=0;i<text.size();){if(text.compare(i,11,"data:image/")==0){auto end=text.find('"',i);if(end!=std::string::npos){images+=4096;i=end;continue;}}auto c=static_cast<unsigned char>(text[i++]);if(c<128)++ascii;else ++other;}return (ascii+2)/3+other+images;}
struct ContextBudget{
  size_t window=32768,output=4096;
  size_t input()const{return window-output-window/10;}
  static size_t number(const std::string& value){if(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Context budgets must be positive integer token counts");size_t used=0;auto n=std::stoull(value,&used);if(!n||used!=value.size()||n>16*1024*1024)throw std::runtime_error("Context budget exceeds 16M tokens");return static_cast<size_t>(n);}
  void validate()const{if(window<4096||window>16*1024*1024||output<256||output>=window-window/10-1024)throw std::runtime_error("Context window must be 4096..16M; output reserve must leave at least 1024 input tokens and 10% safety margin");}
};

// Startup defaults travel together through provider changes and child launches.
struct AgentBudget {
  enum class Option { Steps, Window, Reserve };

  std::optional<uint64_t> max_steps = 64;
  size_t context_window = 32768;
  size_t output_reserve = 0;

  static std::optional<Option> option(const std::string& name) {
    if (name == "--max-steps") return Option::Steps;
    if (name == "--context-window") return Option::Window;
    if (name == "--output-reserve") return Option::Reserve;
    return std::nullopt;
  }

  static std::optional<uint64_t> step_limit(const std::string& value) {
    if (value == "unlimited") return std::nullopt;
    if (!value.empty() && value.size() <= 3 && value.find_first_not_of("0123456789") == std::string::npos) {
      const int limit = std::stoi(value);
      if (limit >= 1 && limit <= 512) return static_cast<uint64_t>(limit);
    }
    throw std::runtime_error("--max-steps must be an integer from 1 to 512 or unlimited");
  }

  ContextBudget context() const {
    return {context_window, output_reserve ? output_reserve : std::min<size_t>(4096, context_window / 8)};
  }

  void append_args(std::vector<std::string>& args) const {
    args.insert(args.end(), {
      "--max-steps", max_steps ? std::to_string(*max_steps) : "unlimited",
      "--context-window", std::to_string(context_window),
      "--output-reserve", std::to_string(context().output),
    });
  }
};
