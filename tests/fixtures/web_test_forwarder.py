"""TEST ONLY: expose a container's loopback gateway to the host HTTPS test proxy."""
import selectors
import socket
import socketserver

class Relay(socketserver.BaseRequestHandler):
    def handle(self):
        with socket.create_connection(('127.0.0.1',8765)) as upstream, selectors.DefaultSelector() as selector:
            selector.register(self.request,selectors.EVENT_READ,upstream)
            selector.register(upstream,selectors.EVENT_READ,self.request)
            while True:
                for key,_ in selector.select(15):
                    data=key.fileobj.recv(65536)
                    if not data:return
                    key.data.sendall(data)
class Server(socketserver.ThreadingTCPServer):
    daemon_threads=True
    allow_reuse_address=True
with Server(('0.0.0.0',9876),Relay) as server:server.serve_forever()
