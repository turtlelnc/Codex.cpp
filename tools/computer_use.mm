// Optional macOS MCP controller. System frameworks only; no model/API credentials.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>

static NSMutableArray *elements;
static NSSet *allowed;
static NSString *snapshotID, *snapshotApp;
static bool dryRun=false;
static CGRect screen;
static int canvasW,canvasH;
static NSString* json(id value){NSData* data=[NSJSONSerialization dataWithJSONObject:value options:0 error:nil];return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];}
static id attr(AXUIElementRef e,CFStringRef key){CFTypeRef v=nullptr;return AXUIElementCopyAttributeValue(e,key,&v)==kAXErrorSuccess?CFBridgingRelease(v):nil;}

static void tree(AXUIElementRef e,NSMutableString* out,int depth){
  if(depth>12 || elements.count>=160 || [attr(e,kAXSubroleAttribute) isEqual:@"AXSecureTextField"])return;NSUInteger n=elements.count;[elements addObject:(__bridge id)e];
  id role=attr(e,kAXRoleAttribute);[out appendFormat:@"%lu %@",(unsigned long)n,role?role:@""];
  for(CFStringRef key:{kAXTitleAttribute,kAXDescriptionAttribute,kAXValueAttribute}){id v=attr(e,key);if([v isKindOfClass:NSString.class] || [v isKindOfClass:NSNumber.class]){
    NSString* text=[v description];[out appendFormat:@" | %@",text.length>600?[text substringToIndex:600]:text];}}
  [out appendString:@"\n"];NSArray* children=attr(e,kAXChildrenAttribute);
  if([children isKindOfClass:NSArray.class])for(id child in children)tree((__bridge AXUIElementRef)child,out,depth+1);
}
static id state(){
  elements=[NSMutableArray new];snapshotID=NSUUID.UUID.UUIDString;NSMutableString* controls=[NSMutableString new];
  if(!dryRun && [allowed containsObject:snapshotApp] && AXIsProcessTrusted()){AXUIElementRef app=AXUIElementCreateApplication([NSRunningApplication runningApplicationsWithBundleIdentifier:snapshotApp].firstObject.processIdentifier);
    NSArray* windows=attr(app,kAXWindowsAttribute);for(id window in windows)tree((__bridge AXUIElementRef)window,controls,0);CFRelease(app);}
  return @{ @"state_id":snapshotID,@"app":snapshotApp,@"controls":controls,@"screen_size":@[@(screen.size.width),@(screen.size.height)],
    @"canvas_size":@[@(canvasW),@(canvasH)],@"dry_run":@(dryRun),@"accessibility":@(dryRun || AXIsProcessTrusted()),@"screen_recording":@(dryRun || CGPreflightScreenCaptureAccess()) };
}
static NSString* validate(NSArray* actions,NSString* observation){
  if(![actions isKindOfClass:NSArray.class] || actions.count>12 || ![@[@"auto",@"text"] containsObject:observation])return @"Use up to 12 background actions; observation must be text or auto. Images are unavailable.";
  for(id a in actions){
    if(![a isKindOfClass:NSDictionary.class] || ![@[@"press_element",@"set_value"] containsObject:a[@"type"]])return @"Only press_element and set_value are supported; global input and activation are disabled.";
    NSNumber* i=a[@"element"];if(![i isKindOfClass:NSNumber.class] || i.doubleValue<0 || i.doubleValue!=i.integerValue)return @"Nonnegative integer element required.";
    if([a[@"type"] isEqual:@"set_value"] && (![a[@"text"] isKindOfClass:NSString.class] || [a[@"text"] length]>4000))return @"set_value requires text, at most 4000 characters.";
  }return nil;
}
static NSDictionary* batch(NSDictionary* args){
  NSArray* actions=args[@"actions"];NSString* error=validate(actions,args[@"observation"]?args[@"observation"]:@"text");NSUInteger done=0;
  NSString* appID=args[@"app"];if(![appID isKindOfClass:NSString.class] || ![allowed containsObject:appID])error=@"Explicit allowlisted app bundle ID required.";
  if(!error && actions.count && (![args[@"state_id"] isEqual:snapshotID] || ![appID isEqual:snapshotApp]))error=@"Stale element reference; observe this app again.";
  if(!error && !dryRun && ![NSRunningApplication runningApplicationsWithBundleIdentifier:appID].count)error=@"Open the allowed app first; this controller does not launch apps.";
  if(!error && !dryRun && !AXIsProcessTrusted())error=@"Accessibility permission missing.";
  if(!error && !dryRun)for(NSDictionary* a in actions){
    NSUInteger i=[a[@"element"] unsignedIntegerValue];if(i>=elements.count){error=@"Element index out of bounds.";break;}
    AXUIElementRef e=(__bridge AXUIElementRef)elements[i];AXError r;
    if([a[@"type"] isEqual:@"set_value"]){Boolean writable=false;AXUIElementIsAttributeSettable(e,kAXValueAttribute,&writable);if(!writable){error=@"Control does not support background value editing.";break;}
      r=AXUIElementSetAttributeValue(e,kAXValueAttribute,(__bridge CFStringRef)a[@"text"]);
    }else r=AXUIElementPerformAction(e,kAXPressAction);
    if(r!=kAXErrorSuccess){error=@"Background accessibility action failed; no input fallback.";break;}++done;
  }
  if(!error && dryRun)done=actions.count;
  snapshotApp=appID;NSDictionary* info=state();
  return @{@"content":@[@{@"type":@"text",@"text":json(@{@"completed_actions":@(done),@"error":error?error:@"",@"state":info})}],@"isError":@(error!=nil)};
}
int main(int argc,char** argv){@autoreleasepool{
  NSMutableSet* apps=[NSMutableSet new];bool check=false;for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--allow-app" && i+1<argc)[apps addObject:[NSString stringWithUTF8String:argv[++i]]];else if(a=="--check")check=true;else if(a=="--dry-run")dryRun=true;else{std::cerr<<"Unknown argument\n";return 2;}}
  allowed=apps;screen=CGDisplayBounds(CGMainDisplayID());double scale=std::min({1.0,1440.0/screen.size.width,900.0/screen.size.height});canvasW=(int)std::round(screen.size.width*scale);canvasH=(int)std::round(screen.size.height*scale);
  if(check){std::cout<<[json(@{@"accessibility":@(AXIsProcessTrusted()),@"screen_recording":@(CGPreflightScreenCaptureAccess()),@"canvas_size":@[@(canvasW),@(canvasH)]}) UTF8String]<<'\n';return 0;}
  if(!apps.count){std::cerr<<"Explicit --allow-app BUNDLE_ID required. Use --check to inspect permissions.\n";return 2;}
  NSDictionary* tool=@{@"name":@"computer_batch",@"description":@"Background macOS accessibility. Specify allowlisted app bundle ID. Observe with actions=[]; then use state_id and element indices for press_element or set_value(text, replaces value). No global mouse, keyboard, focus, app activation or image capture. OS action acceptance is not proof of task success: inspect resulting controls. An app may itself activate a window.",
    @"inputSchema":@{@"type":@"object",@"properties":@{@"app":@{@"type":@"string"},@"actions":@{@"type":@"array",@"maxItems":@12,@"items":@{@"type":@"object"}},@"observation":@{@"type":@"string",@"enum":@[@"auto",@"text"]},@"state_id":@{@"type":@"string"}},@"required":@[@"app",@"actions"],@"additionalProperties":@NO}};
  std::string line;while(std::getline(std::cin,line)){@autoreleasepool{NSData* data=[NSData dataWithBytes:line.data() length:line.size()];id request=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];if(![request isKindOfClass:NSDictionary.class] || !request[@"id"])continue;
    id result=@{};NSString* method=request[@"method"];
    if([method isEqual:@"initialize"])result=@{@"protocolVersion":@"2025-11-25",@"capabilities":@{@"tools":@{}},@"serverInfo":@{@"name":@"native-computer-use",@"version":@"0.1"}};
    else if([method isEqual:@"tools/list"])result=@{@"tools":@[tool]};
    else if([method isEqual:@"tools/call"]){NSDictionary* params=[request[@"params"] isKindOfClass:NSDictionary.class]?request[@"params"]:@{};result=[params[@"name"] isEqual:@"computer_batch"] && [params[@"arguments"] isKindOfClass:NSDictionary.class]?batch(params[@"arguments"]):@{@"isError":@YES,@"content":@[@{@"type":@"text",@"text":@"Unknown tool or invalid arguments."}]};}
    std::cout<<[json(@{@"jsonrpc":@"2.0",@"id":request[@"id"],@"result":result}) UTF8String]<<std::endl;
  }}return 0;
}}
