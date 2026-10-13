#define main controller_main
#include "../tools/computer_use.mm"
#undef main
#include <cassert>
int main(){@autoreleasepool{
  dryRun=true;takeover=true;allowed=[NSSet setWithObject:@"test.app"];
  shield=[CodexShield new];
  NSDictionary* request=@{@"app":@"test.app",@"window":@"Test",@"actions":@[]};
  assert(![batch(request)[@"isError"] boolValue]);
  [shield stop:nil];assert(takeoverStopped);
  for(int n=0;n<2;++n){NSDictionary* result=batch(request);assert([result[@"isError"] boolValue]);
    NSDictionary* text=[NSJSONSerialization JSONObjectWithData:[result[@"content"][0][@"text"] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    assert([text[@"takeover"] isEqual:@"stopped"]);assert([text[@"completed_actions"] integerValue]==0);
    assert([text[@"error"] containsString:@"User stopped"]);
  }
  std::cout<<"PASS user stop persists across observations; no automatic resume\n";
}return 0;}
