#define main codex_runtime_main
#include "../codex.cpp"
#undef main
#include <cassert>

int main(int argc,char** argv){
  assert(argc==3);const fs::path root=argv[1];ApiClient api;
  api.base_url=argv[2];api.model="review-test";api.reasoning_effort="low";api.no_api_key=true;
  DesignReviewTool review(api);
  const std::string args=R"({"decision":"review","task":"Check the quiz flow","paths":["index.html"]})";
  assert(review.run(args,root).exit_code==126);
  review.enabled=true;
  assert(review.run(R"({"decision":"skip","task":"Only changing a label","paths":[]})",root).exit_code==0);assert(review.skipped);review.skipped=false;
  assert(review.run(R"({"decision":"review","task":"test","paths":["../outside.txt"]})",root).exit_code==2);
  assert(review.run(R"({"decision":"review","task":"test","paths":["link.html"]})",root).exit_code==2);
  assert(review.run(R"({"decision":"review","task":"test","paths":["large.txt"]})",root).exit_code==2);
  assert(review.run(R"({"decision":"review","task":"test","paths":["binary.dat"]})",root).exit_code==2);
  assert(review.rounds==0);
  for(int i=0;i<2;++i){if(i){assert(review.run(R"({"decision":"skip","task":"Try skipping follow-up","paths":[]})",root).exit_code==2);assert(review.pending);}auto result=review.run(args,root);assert(!review.pending);if(result.exit_code)std::cerr<<result.output<<"\n";assert(result.exit_code==0);assert(result.output.find("confirmed test finding")!=std::string::npos);}
  assert(review.run(args,root).exit_code==2);
  review.rounds=0;assert(review.run(args,root).exit_code==3);assert(review.rounds==1);
  std::cout<<"Design review isolation, limits and failures passed\n";
}
