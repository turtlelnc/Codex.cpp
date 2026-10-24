"""Local PTY provider selection regression; no credentials or model calls."""
import os,pty,fcntl,termios,struct,subprocess,tempfile,time,select,sys
from pathlib import Path
BINARY=str(Path(sys.argv[1] if len(sys.argv)>1 else "./codex-cpp").resolve())
def run(cols,rows):
    with tempfile.TemporaryDirectory() as d:
        master,slave=pty.openpty();fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,0,0))
        def terminal():
            os.setsid();fcntl.ioctl(slave,termios.TIOCSCTTY,0)
        p=subprocess.Popen([BINARY,'-i','--root',d,'--provider','openai','--no-api-key'],stdin=slave,stdout=slave,stderr=slave,preexec_fn=terminal,env=dict(os.environ,TERM='xterm-256color',CODEX_CPP_HOME=d+'/home'))
        os.close(slave);output=bytearray()
        def until(text):
            end=time.monotonic()+5
            while text not in output and time.monotonic()<end:
                if select.select([master],[],[],.1)[0]:
                    try:output.extend(os.read(master,65536))
                    except OSError:break

            assert text in output,bytes(output[-1500:])
        try:
            until(b'Ask Codex');time.sleep(.3)
            os.write(master,b'/provider ');time.sleep(.1);os.write(master,b'\r');until(b'Choose provider')
            os.write(master,b'\x1b[B\r');until(b'Provider set to deepseek')
            output.clear();os.write(master,b'/provider ');time.sleep(.1);os.write(master,b'\r');until(b'Choose provider')
            os.write(master,b'\x1b');time.sleep(.3)
            output.clear();os.write(master,b'/provider ');time.sleep(.1);os.write(master,b'\r');until(b'DeepSeek API (current)');os.write(master,b'\x1b[A\r');until(b'Provider set to openai');time.sleep(.3)
            os.write(master,b'/quit\r');p.wait(timeout=5)
            assert p.returncode==0
            print('PASS real TUI picker selects DeepSeek; Escape preserves selection; switch back to OpenAI')
        finally:
            if p.poll() is None:p.kill();p.wait()
            os.close(master)

run(100,24)
run(48,16)
