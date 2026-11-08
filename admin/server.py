import argparse
import json
import logging
from logging.handlers import RotatingFileHandler
import secrets
import tempfile
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler , ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs , urlsplit

from database import Database
from device import Device
from validation import Problem , readcsv , today

ROOT = Path(__file__).resolve().parent


class Application:
    def __init__(self , path):
        self.db = Database(path)
        self.device = Device(self.db)
        self.token = secrets.token_urlsafe(32)
        self.shutdown = threading.Event()


def handler(app):
    class Handler(BaseHTTPRequestHandler):
        server_version = "Attendance/1.0"

        def log_message(self , fmt , *args):
            logging.info(fmt , *args)

        def send(self , data , content_type = "application/json; charset=utf-8" , status = 200 , filename = None):
            if isinstance(data , (dict , list)):
                data = json.dumps(data , ensure_ascii = False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type" , content_type)
            self.send_header("Content-Length" , str(len(data)))
            self.send_header("Cache-Control" , "no-store")
            self.send_header("X-Content-Type-Options" , "nosniff")
            self.send_header("Content-Security-Policy" , "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
            if filename:
                self.send_header("Content-Disposition" , 'attachment; filename="' + filename + '"')
            self.end_headers()
            self.wfile.write(data)

        def route(self , method):
            allowed = {f"127.0.0.1:{self.server.server_port}" , f"localhost:{self.server.server_port}"}
            if self.headers.get("Host") not in allowed:
                raise Problem("仅允许通过本机地址访问" , 403)
            target = urlsplit(self.path)
            path = target.path
            query = parse_qs(target.query)
            arg = lambda key , default = "": query.get(key , [default])[0]
            if method == "GET":
                if path == "/api/bootstrap":
                    return self.send({"token": app.token , "today": today() , "version": "1.0.0"})
                if path == "/api/state":
                    return self.send(app.device.snapshot())
                if path == "/api/ports":
                    return self.send(app.device.ports())
                if path == "/api/overview":
                    return self.send(app.db.overview(arg("day" , today())))
                if path == "/api/members":
                    return self.send(app.db.members(arg("q")))
                if path in ("/api/records" , "/api/export"):
                    start , end , search = arg("start" , today()) , arg("end" , today()) , arg("q")
                    if path.endswith("export"):
                        return self.send(app.db.export(start , end , search) , "text/csv; charset=utf-8" ,
                                         filename = f"attendance-{start}-{end}.csv")
                    rows = app.db.records(start , end , search)
                    try:
                        page = max(1 , int(arg("page" , "1")))
                    except ValueError:
                        raise Problem("页码应为整数") from None
                    return self.send({"rows": rows[(page - 1) * 25:page * 25] , "total": len(rows) , "page": page})
                if path == "/api/backup":
                    with tempfile.TemporaryDirectory(prefix = "nfc-backup-") as folder:
                        backup = Path(folder) / "attendance.sqlite3"
                        app.db.backup(backup)
                        return self.send(backup.read_bytes() , "application/vnd.sqlite3" , filename = "attendance-backup-" + today() + ".sqlite3")
                assets = {"/": "index.html" , "/app.js": "app.js" , "/network.js": "network.js" , "/style.css": "style.css" , "/favicon.svg": "favicon.svg"}
                if path in assets:
                    file = ROOT / "static" / assets[path]
                    types = {".html": "text/html" , ".css": "text/css" , ".js": "text/javascript" , ".svg": "image/svg+xml"}
                    return self.send(file.read_bytes() , types[file.suffix] + "; charset=utf-8")
                raise Problem("页面不存在" , 404)
            if self.headers.get("X-Admin-Token") != app.token:
                raise Problem("页面会话已失效，请刷新页面" , 403)
            origin = self.headers.get("Origin")
            if origin and origin not in {"http://" + host for host in allowed}:
                raise Problem("请求来源不受支持" , 403)
            try:
                size = int(self.headers.get("Content-Length" , "0"))
            except ValueError:
                raise Problem("请求长度无效") from None
            if size < 0 or size > 3 * 1024 * 1024:
                raise Problem("请求超过 3 MiB" , 413)
            raw = self.rfile.read(size)
            if path == "/api/import":
                return self.send(app.db.ingest(readcsv(raw) , "CSV 文件导入"))
            try:
                data = json.loads(raw or b"{}")
                if not isinstance(data , dict):
                    raise ValueError()
            except (ValueError , UnicodeDecodeError):
                raise Problem("请求应为 JSON 对象") from None
            if path == "/api/device/connect":
                return self.send(app.device.connect(data.get("port")))
            if path == "/api/device/disconnect":
                return self.send(app.device.disconnect())
            if path == "/api/device/members":
                return self.send(app.device.members())
            if path == "/api/device/sync":
                return self.send(app.device.sync(data.get("day" , today())))
            if path == "/api/device/bind":
                return self.send(app.device.bind(data.get("uid") , data.get("name")))
            if path == "/api/device/clock":
                return self.send(app.device.clock())
            if path == "/api/shutdown":
                self.send({"message": "后台正在停止"})
                threading.Thread(target = self.server.shutdown , daemon = True).start()
                return
            if path == "/api/member/edit":
                app.db.edit(data.get("uid") , data)
                return self.send({"message": "人员备注已保存到本机"})
            raise Problem("接口不存在" , 404)

        def dispatch(self , method):
            try:
                self.route(method)
            except Problem as exc:
                self.send({"error": str(exc)} , status = exc.status)
            except (BrokenPipeError , ConnectionResetError):
                pass
            except Exception:
                logging.exception("Unhandled request")
                self.send({"error": "服务处理失败，请查看 data/server.log"} , status = 500)

        def do_GET(self):
            self.dispatch("GET")

        def do_POST(self):
            self.dispatch("POST")

    return Handler


def sol():
    parser = argparse.ArgumentParser(description = "NFC 签到终端本地管理后台")
    parser.add_argument("--port" , type = int , default = 8765)
    parser.add_argument("--db" , default = str(ROOT / "data" / "attendance.sqlite3"))
    parser.add_argument("--open" , action = "store_true")
    args = parser.parse_args()
    logpath = Path(args.db).parent
    logpath.mkdir(parents = True , exist_ok = True)
    logging.basicConfig(level = logging.INFO , handlers = [RotatingFileHandler(logpath / "server.log" ,
                        maxBytes = 2 * 1024 * 1024 , backupCount = 3 , encoding = "utf-8")])
    app = Application(args.db)
    server = ThreadingHTTPServer(("127.0.0.1" , args.port) , handler(app))
    worker = threading.Thread(target = app.device.maintenance , args = (app.shutdown ,) , daemon = True)
    worker.start()
    if args.open:
        threading.Thread(target = lambda: webbrowser.open(f"http://127.0.0.1:{args.port}") , daemon = True).start()
    print(f"NFC attendance admin v1.0.0\nhttp://127.0.0.1:{args.port}\nKeep this window open. Press Ctrl+C to stop." , flush = True)
    try:
        server.serve_forever(poll_interval = 0.3)
    except KeyboardInterrupt:
        pass
    finally:
        app.shutdown.set()
        app.device.disconnect()
        server.server_close()


if __name__ == "__main__":
    sol()
