"""Local fake Responses provider; all keys used here are test fixtures."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading

seen=[]
lock=threading.Lock()
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):
        pass
    def do_GET(self):
        data=json.dumps(seen).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_POST(self):
        request=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        items=request.get('input',[])
        index=max((i for i,item in enumerate(items) if item.get('role')=='user'),default=-1)
        prompt=json.dumps(items[index].get('content','')) if index>=0 else ''
        label='SECOND' if 'SECOND' in prompt else 'FIRST' if 'FIRST' in prompt else 'PERSONAL'
        with lock:seen.append({'label':label,'authorization':self.headers.get('Authorization')})
        finished=any(item.get('type')=='function_call_output' for item in items[index+1:])
        if label!='PERSONAL' and not finished:
            output=[{'type':'function_call','call_id':'write-'+label,'name':'write_file','arguments':json.dumps({'path':'queue.txt','content':label.lower()})}]
        else:
            output=[{'type':'message','role':'assistant','content':[{'type':'output_text','text':'DONE '+label}]}]
        data=json.dumps({'id':'fixture','output':output}).encode()
        self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
ThreadingHTTPServer(('127.0.0.1',8123),Handler).serve_forever()
