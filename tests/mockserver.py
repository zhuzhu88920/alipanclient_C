"""本地 mock：仅用于验证列目录与下载链路，不作为交付内容。"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FILE_CONTENT = b"hello alipanclient_C mock\r\n" * 512
BIG_SIZE = 6 * 1024 * 1024 + 123
BIG_CONTENT = (bytes(range(256)) * ((BIG_SIZE // 256) + 1))[:BIG_SIZE]


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        try:
            req = json.loads(raw.decode("utf-8"))
        except Exception:
            req = {}
        if self.path.endswith("/user/getDriveInfo"):
            return self._json({"user_id": "u1", "default_drive_id": "d1"})
        if self.path.endswith("/openFile/list"):
            return self._json({"items": [
                {"file_id": "f1", "name": "报告 2026.pdf", "type": "file", "size": len(FILE_CONTENT)},
                {"file_id": "f2", "name": "OS", "type": "folder", "size": 0},
                {"file_id": "f3", "name": "notes.txt", "type": "file", "size": len(FILE_CONTENT)},
                {"file_id": "f4", "name": "big.bin", "type": "file", "size": BIG_SIZE},
                {"file_id": "f5", "name": "norange.bin", "type": "file", "size": BIG_SIZE},
            ], "next_marker": ""})
        if self.path.endswith("/getDownloadUrl"):
            if req.get("file_id") == "f4":
                path = "/bigfile"
            elif req.get("file_id") == "f5":
                path = "/norange"
            else:
                path = "/file"
            size = len(FILE_CONTENT) if path == "/file" else BIG_SIZE
            return self._json({"url": "http://127.0.0.1:%d%s" % (self.server.server_port, path),
                               "method": "GET", "size": size})
        return self._json({"error": "not found"}, 404)

    def _serve_data(self, data, honor_range=True):
        rng = self.headers.get("Range")
        status = 200
        if honor_range and rng and rng.startswith("bytes="):
            spec = rng[6:]
            a, _, b = spec.partition("-")
            start = int(a) if a else 0
            end = int(b) + 1 if b else len(data)
            data = data[start:end]
            status = 206
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/auth/tickstep/aliyunpan/token/qrcode/create"):
            return self._json({"code": 0, "msg": "请求完成", "data": {
                "tokenUrl": "https://api.tickstep.com/pages/tickstep/aliyunpan/qrcode/login?tokenId=mock-ticket-001",
                "expiredTime": 300}})
        if self.path.startswith("/auth/tickstep/aliyunpan/token/common/"):
            return self._json({"code": 0, "msg": "请求完成", "data": {
                "openapi": {"accessToken": "mock-open-token", "expired": 4102444800},
                "webapi": {"accessToken": "mock-web-token", "expired": 4102444800}}})
        if self.path.startswith("/bigfile"):
            return self._serve_data(BIG_CONTENT)
        if self.path.startswith("/norange"):
            return self._serve_data(BIG_CONTENT, honor_range=False)
        if self.path.startswith("/file"):
            return self._serve_data(FILE_CONTENT)
        self.send_response(404)
        self.end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18765
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
