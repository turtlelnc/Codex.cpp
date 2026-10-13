// Opt-in visual mouse shield, not an exclusive window lock. Main thread only.
static bool takeover=false, takeoverStopped=false;
static id takeoverWindow;
static NSPanel* takeoverPanel;
static NSString* takeoverApp;
@interface CodexShieldPanel:NSPanel
@end
@implementation CodexShieldPanel
-(BOOL)canBecomeKeyWindow{return NO;}
-(BOOL)canBecomeMainWindow{return NO;}
@end
@interface CodexShield:NSObject
-(void)stop:(id)sender;
-(void)tick:(id)sender;
@end
static CodexShield* shield;
@implementation CodexShield
-(void)stop:(id)sender{(void)sender;takeoverStopped=true;[takeoverPanel orderOut:nil];}
-(void)tick:(id)sender{(void)sender;
  if(!takeover || takeoverStopped || !takeoverWindow){[takeoverPanel orderOut:nil];return;}
  NSRunningApplication* app=NSWorkspace.sharedWorkspace.frontmostApplication;
  if(![app.bundleIdentifier isEqual:takeoverApp]){[takeoverPanel orderOut:nil];return;}
  AXUIElementRef appAX=AXUIElementCreateApplication(app.processIdentifier);AXUIElementSetMessagingTimeout(appAX,.15);
  id focused=attr(appAX,kAXFocusedWindowAttribute);CFRelease(appAX);
  if(!focused || !CFEqual((__bridge CFTypeRef)focused,(__bridge CFTypeRef)takeoverWindow)){[takeoverPanel orderOut:nil];return;}
  id p=attr((__bridge AXUIElementRef)takeoverWindow,kAXPositionAttribute),s=attr((__bridge AXUIElementRef)takeoverWindow,kAXSizeAttribute);
  CGPoint position;CGSize size;
  if(!p || !s || CFGetTypeID((__bridge CFTypeRef)p)!=AXValueGetTypeID() || CFGetTypeID((__bridge CFTypeRef)s)!=AXValueGetTypeID() || !AXValueGetValue((__bridge AXValueRef)p,kAXValueTypeCGPoint,&position) || !AXValueGetValue((__bridge AXValueRef)s,kAXValueTypeCGSize,&size)){[self stop:nil];return;}
  NSRect frame=NSMakeRect(position.x,NSMaxY(NSScreen.screens.firstObject.frame)-position.y-size.height,size.width,size.height);
  [takeoverPanel setFrame:frame display:YES];[takeoverPanel orderFrontRegardless];
}
@end
static void attachShield(id window,NSString* appID){
  if(!takeover || takeoverStopped || !window)return;
  takeoverWindow=window;takeoverApp=appID;
  if(!takeoverPanel){
    takeoverPanel=[[CodexShieldPanel alloc] initWithContentRect:NSMakeRect(0,0,400,200) styleMask:NSWindowStyleMaskBorderless|NSWindowStyleMaskNonactivatingPanel backing:NSBackingStoreBuffered defer:NO];
    takeoverPanel.title=@"Codex window takeover";takeoverPanel.opaque=NO;takeoverPanel.backgroundColor=[NSColor colorWithCalibratedWhite:.18 alpha:.83];
    takeoverPanel.level=NSFloatingWindowLevel;takeoverPanel.hidesOnDeactivate=NO;takeoverPanel.releasedWhenClosed=NO;takeoverPanel.hasShadow=NO;
    NSView* view=takeoverPanel.contentView;
    NSTextField* label=[NSTextField labelWithString:@"Codex 正在操作此窗口"];label.textColor=NSColor.whiteColor;label.font=[NSFont boldSystemFontOfSize:18];label.translatesAutoresizingMaskIntoConstraints=NO;
    NSButton* stop=[NSButton buttonWithTitle:@"停止接管" target:shield action:@selector(stop:)];stop.translatesAutoresizingMaskIntoConstraints=NO;
    [view addSubview:label];[view addSubview:stop];
    [NSLayoutConstraint activateConstraints:@[[label.centerXAnchor constraintEqualToAnchor:view.centerXAnchor],[label.centerYAnchor constraintEqualToAnchor:view.centerYAnchor constant:-20],[stop.centerXAnchor constraintEqualToAnchor:view.centerXAnchor],[stop.topAnchor constraintEqualToAnchor:label.bottomAnchor constant:20]]];
  }
  [shield tick:nil];
}
