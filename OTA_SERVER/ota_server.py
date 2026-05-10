import http.server, socketserver
PORT = 8080
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, f, *a):
        print(f"[OTA] {self.client_address[0]} {f % a}")
print(f"OTA server on port {PORT}")
with socketserver.TCPServer(("", PORT), H) as s:
    s.serve_forever()