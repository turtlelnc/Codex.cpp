"""Opt-in macOS Accessibility test using two background-only fixture windows.
Usage: python3 tests/computer_use_live.py /absolute/path/to/codex-computer-use
Needs Accessibility permission; no real model calls or global input.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

class Client:
    def __init__(self, binary):
        self.proc = subprocess.Popen([binary, '--allow-app', 'net.codex.backgroundfixture'],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.counter = 0
    def call(self, title, actions=None, state=''):
        self.counter += 1
        request = {'jsonrpc': '2.0', 'id': self.counter, 'method': 'tools/call',
                   'params': {'name': 'computer_batch', 'arguments': {
                       'app': 'net.codex.backgroundfixture', 'window': title,
                       'actions': actions or [], 'state_id': state}}}
        self.proc.stdin.write(json.dumps(request)+'\n')
        self.proc.stdin.flush()
        result = json.loads(self.proc.stdout.readline())['result']
        return result, json.loads(result['content'][0]['text'])
    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=5)

def field(state):
    return int(next(x for x in state['state']['controls'].splitlines() if 'AXTextField' in x).split()[0])

with tempfile.TemporaryDirectory(prefix='codex-controls-') as folder:
    root = Path(folder)
    bundle = root/'Fixture.app/Contents'
    (bundle/'MacOS').mkdir(parents=True)
    (bundle/'Info.plist').write_text('''<?xml version="1.0"?><plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>net.codex.backgroundfixture</string>
<key>CFBundleExecutable</key><string>fixture</string><key>LSUIElement</key><true/>
</dict></plist>''')
    source = root/'fixture.mm'
    source.write_text('''#import <AppKit/AppKit.h>
@interface Receiver:NSObject
@property NSTextField* result;
-(void)submit:(id)sender;
@end
@implementation Receiver
-(void)submit:(id)sender{(void)sender;self.result.stringValue=@"submitted";}
@end
int main(){@autoreleasepool{
[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
NSMutableArray* keep=[NSMutableArray new];
for(NSString* title in @[@"Selected fixture",@"Other fixture"]){
NSWindow* w=[[NSWindow alloc] initWithContentRect:NSMakeRect(100,100,400,120) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
w.title=title;NSTextField* field=[[NSTextField alloc] initWithFrame:NSMakeRect(20,40,360,30)];
field.stringValue=[title isEqual:@"Selected fixture"]?@"baseline":@"OTHER_WINDOW_SENTINEL";
[w.contentView addSubview:field];Receiver* h=[Receiver new];h.result=[NSTextField labelWithString:@"not submitted"];h.result.frame=NSMakeRect(20,5,180,20);[w.contentView addSubview:h.result];
NSButton* button=[[NSButton alloc] initWithFrame:NSMakeRect(220,5,100,25)];button.title=@"Submit";button.target=h;button.action=@selector(submit:);[w.contentView addSubview:button];[keep addObject:h];[keep addObject:w];[w orderBack:nil];}
[NSApp run];}return 0;}
''')
    executable = bundle/'MacOS/fixture'
    subprocess.run(['clang++', '-fobjc-arc', str(source), '-framework', 'AppKit', '-o', str(executable)], check=True)
    app = subprocess.Popen([str(executable)])
    clients = [Client(str(Path(sys.argv[1]).resolve())) for _ in range(2)]
    first, external = clients
    try:
        time.sleep(2)
        title = 'Selected fixture'
        result, observed = first.call(title)
        assert not result['isError'], observed['error']
        assert 'OTHER_WINDOW_SENTINEL' not in observed['state']['controls']
        result, current = external.call(title)
        edited = 'external edit: 请保留我的修改'
        result, current = external.call(title, [{'type': 'set_value', 'element': field(current), 'text': edited}], current['state']['state_id'])
        assert not result['isError'], current['error']
        result, conflict = first.call(title, [{'type': 'set_value', 'element': field(observed), 'text': 'must not overwrite'}], observed['state']['state_id'])
        assert result['isError'] and conflict['completed_actions'] == 0
        assert conflict['conflict']['kind'] == 'control_changed'
        assert conflict['conflict']['previous_value'] == 'baseline'
        assert conflict['conflict']['current_value'] == edited
        assert edited in conflict['state']['controls']
        assert 'must not overwrite' not in conflict['state']['controls']
        result, again = first.call(title, [{'type': 'set_value', 'element': field(observed), 'text': 'stale retry'}], observed['state']['state_id'])
        assert result['isError'] and again['completed_actions'] == 0
        result, button_observed = first.call(title)
        button = int(next(x for x in button_observed['state']['controls'].splitlines() if 'AXButton' in x).split()[0])
        result, changed = external.call(title)
        result, changed = external.call(title, [{'type': 'set_value', 'element': field(changed), 'text': 'changed before submission: 999'}], changed['state']['state_id'])
        assert not result['isError']
        result, guarded = first.call(title, [{'type': 'press_element', 'element': button}], button_observed['state']['state_id'])
        assert result['isError'] and guarded['completed_actions'] == 0
        assert guarded['conflict']['kind'] == 'window_changed'
        assert 'not submitted' in guarded['state']['controls']
        assert guarded['conflict']['current_value'] == 'changed before submission: 999'
        # Explicit fresh observation can allow submission; the guard is not an approval lock.
        result, fresh = first.call(title)
        button = int(next(x for x in fresh['state']['controls'].splitlines() if 'AXButton' in x).split()[0])
        result, submitted = first.call(title, [{'type': 'press_element', 'element': button}], fresh['state']['state_id'])
        assert not result['isError'], submitted['error']
        assert 'AXStaticText | submitted' in submitted['state']['controls']
        result, other = external.call('Other fixture')
        assert not result['isError'] and 'OTHER_WINDOW_SENTINEL' in other['state']['controls']
        result, missing = first.call('Missing fixture')
        assert result['isError'] and not missing['state']['controls']
        print('PASS live window isolation, concurrent-edit conflict, refreshed context, stale retry, form-change button guard, unchanged button success and missing-window errors')
    finally:
        for client in clients:
            client.close()
        app.terminate()
        app.wait(timeout=5)
