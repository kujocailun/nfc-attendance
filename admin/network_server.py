import argparse
from contextlib import asynccontextmanager
from dataclasses import dataclass
import json
import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path
import secrets
import tempfile
import threading
import time
from urllib.parse import urlsplit

import jwt
from fastapi import FastAPI , Request
from fastapi.responses import JSONResponse , Response
from starlette.concurrency import run_in_threadpool
import uvicorn

from device import Device
from netstore import NetStore
from validation import Problem , nowtext , readcsv , today

ROOT = Path(__file__).resolve().parent


@dataclass
class Settings:
    admin_host: str = "checkin.example.com"
    device_host: str = "device-api.example.com"
    access_issuer: str = ""
    access_audience: str = ""
    admin_email: str = "admin@example.com"
    local_admin: bool = True
    port: int = 18080

    def validate(self):
        for host in (self.admin_host , self.device_host):
            if not host or urlsplit("https://" + host).netloc != host or "/" in host or ":" in host:
                raise ValueError("Invalid hostname")
        if self.admin_host == self.device_host:
            raise ValueError("Admin and device hosts must differ")
        if not self.local_admin and (not self.access_issuer or not self.access_audience or not self.admin_email):
            raise ValueError("Production requires Access issuer, audience and allowed email")
        if self.access_issuer and (not self.access_issuer.startswith("https://") or
                                   not self.access_issuer.endswith(".cloudflareaccess.com")):
            raise ValueError("Access issuer must be your HTTPS Cloudflare Access team URL without trailing slash")


class AccessVerifier:
    def __init__(self , settings):
        self.settings = settings
        self.client = jwt.PyJWKClient(settings.access_issuer + "/cdn-cgi/access/certs" , timeout = 5) if settings.access_issuer else None

    def verify(self , token):
        if not self.client or not token or len(token) > 16384:
            raise Problem("请通过 Cloudflare Access 登录管理域名" , 401)
        try:
            key = self.client.get_signing_key_from_jwt(token)
            claims = jwt.decode(token , key.key , algorithms = ["RS256"] , audience = self.settings.access_audience ,
                                issuer = self.settings.access_issuer , options = {"require": ["exp" , "iat" , "iss" , "aud" , "sub" , "email"]})
            if claims.get("email" , "").casefold() != self.settings.admin_email.casefold() or claims.get("type") != "app":
                raise ValueError("Identity is not allowed")
        except (jwt.PyJWTError , ValueError):
            raise Problem("登录身份无效或已过期" , 401) from None


def create_app(path , settings = None , verifier = None):
    settings = settings or Settings()
    settings.validate()
    store = NetStore(path)
    device = Device(store)
    token = secrets.token_urlsafe(32)
    verifier = verifier or AccessVerifier(settings)
    shutdown = threading.Event()
    attempts = {}

    def backups():
        while not shutdown.is_set():
            try:
                folder = Path(path).parent / "backups"
                folder.mkdir(exist_ok = True)
                target = folder / ("attendance-" + today() + ".sqlite3")
                if not target.exists():
                    temporary = target.with_suffix(".pending")
                    store.backup(temporary)
                    temporary.replace(target)
                for old in sorted(folder.glob("attendance-????-??-??.sqlite3"))[:-14]:
                    old.unlink()
            except Exception:
                logging.exception("Daily database backup failed")
            shutdown.wait(3600)

    @asynccontextmanager
    async def lifespan(app):
        worker = threading.Thread(target = device.maintenance , args = (shutdown ,) , daemon = True)
        worker.start()
        backup_worker = threading.Thread(target = backups , daemon = True)
        backup_worker.start()
        try:
            yield
        finally:
            shutdown.set()
            await run_in_threadpool(device.disconnect)
            await run_in_threadpool(worker.join , 3)
            await run_in_threadpool(backup_worker.join , 3)

    app = FastAPI(docs_url = None , redoc_url = None , openapi_url = None , lifespan = lifespan)
    app.state.store , app.state.device , app.state.settings = store , device , settings

    @app.exception_handler(Problem)
    async def problem(request , exc):
        return JSONResponse({"error": str(exc)} , status_code = exc.status)

    @app.middleware("http")
    async def guard(request , call_next):
        try:
            host = request.headers.get("host" , "").lower()
            localhosts = {f"127.0.0.1:{settings.port}" , f"localhost:{settings.port}"}
            local = host in localhosts and request.client and request.client.host in {"127.0.0.1" , "::1" , "testclient"}
            # 公网隧道不能借改 Host 命中本地开发入口。
            local = local and not any(key in request.headers for key in ("cf-connecting-ip" , "cf-access-jwt-assertion" , "cf-ray"))
            network = request.url.path.startswith("/device/v1/")
            if network:
                if host != settings.device_host and not (local and settings.local_admin):
                    raise Problem("设备接口仅在设备域名提供" , 404)
                if request.url.path != "/device/v1/pair":
                    ident = request.headers.get("x-device-id" , "")
                    auth = request.headers.get("authorization" , "")
                    if not auth.startswith("Bearer "):
                        raise Problem("缺少设备凭据" , 401)
                    await run_in_threadpool(store.authenticate , ident , auth[7:])
                    request.state.device_id = ident
            else:
                if host == settings.admin_host:
                    await run_in_threadpool(verifier.verify , request.headers.get("cf-access-jwt-assertion" , ""))
                elif not (local and settings.local_admin):
                    raise Problem("管理入口未授权" , 403)
                if request.method != "GET":
                    if not secrets.compare_digest(request.headers.get("x-admin-token" , "") , token):
                        raise Problem("页面会话已失效，请刷新" , 403)
                    origin = request.headers.get("origin")
                    expected = "https://" + settings.admin_host if host == settings.admin_host else "http://" + host
                    if origin and origin != expected:
                        raise Problem("请求来源无效" , 403)
            # 流式计数，不信任 Content-Length，避免分块请求绕过上限。
            if request.method != "GET":
                size , chunks = 0 , []
                async for chunk in request.stream():
                    size += len(chunk)
                    if size > (128 * 1024 if network else 3 * 1024 * 1024):
                        raise Problem("请求过大" , 413)
                    chunks.append(chunk)
                request._body = b"".join(chunks)
            response = await call_next(request)
        except Problem as exc:
            response = JSONResponse({"error": str(exc)} , status_code = exc.status)
        except Exception:
            logging.exception("Network request failed")
            response = JSONResponse({"error": "服务处理失败，请查看服务器日志"} , status_code = 500)
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
        return response

    async def body(request):
        try:
            data = await request.json()
            if not isinstance(data , dict):
                raise ValueError()
            return data
        except (ValueError , UnicodeDecodeError):
            raise Problem("请求应为 JSON 对象") from None

    @app.post("/device/v1/{action}")
    async def device_api(action: str , request: Request):
        data = await body(request)
        if action == "pair":
            # 配对码有 64 位熵，另限制单来源尝试次数，避免无界内存增长。
            key = request.headers.get("cf-connecting-ip") or request.client.host
            now = time.monotonic()
            for old in list(attempts):
                if now - attempts[old][0] > 60:
                    del attempts[old]
            if key not in attempts and len(attempts) >= 4096:
                raise Problem("配对请求过多，请稍后再试" , 429)
            stamp , count = attempts.get(key , (now , 0))
            attempts[key] = (stamp , count + 1)
            if count >= 12:
                raise Problem("配对尝试过多，请一分钟后重试" , 429)
            return await run_in_threadpool(store.pair , data)
        ident = request.state.device_id
        handlers = {"heartbeat": store.heartbeat , "events": store.upload , "command-ack": store.ackcommand , "roster": store.netroster}
        if action not in handlers:
            raise Problem("设备接口不存在" , 404)
        return await run_in_threadpool(handlers[action] , ident , data)

    @app.get("/device/v1/commands")
    def commands(request: Request):
        return store.commands(request.state.device_id)

    @app.get("/device/v1/time")
    def clock():
        return {"time": nowtext()}

    @app.get("/api/{action:path}")
    def admin_get(action: str , request: Request):
        query = request.query_params
        if action == "bootstrap":
            return {"token": token , "today": today() , "version": "2.1.0" , "network": True}
        if action == "state":
            return device.snapshot()
        if action == "ports":
            return device.ports()
        if action == "network":
            return store.devices()
        if action == "overview":
            return store.overview(query.get("day" , today()))
        if action == "members":
            return store.members(query.get("q" , ""))
        if action in ("records" , "export"):
            start , end , search = query.get("start" , today()) , query.get("end" , today()) , query.get("q" , "")
            if action == "export":
                return Response(store.export(start , end , search) , media_type = "text/csv; charset=utf-8" ,
                                headers = {"Content-Disposition": f'attachment; filename="attendance-{start}-{end}.csv"'})
            rows = store.records(start , end , search)
            try:
                page = max(1 , int(query.get("page" , "1")))
            except ValueError:
                raise Problem("页码应为整数") from None
            return {"rows": rows[(page - 1) * 25:page * 25] , "total": len(rows) , "page": page}
        if action == "backup":
            with tempfile.TemporaryDirectory() as folder:
                target = Path(folder) / "backup.sqlite3"
                store.backup(target)
                return Response(target.read_bytes() , media_type = "application/vnd.sqlite3" ,
                                headers = {"Content-Disposition": 'attachment; filename="attendance-backup.sqlite3"'})
        raise Problem("接口不存在" , 404)

    @app.post("/api/{action:path}")
    async def admin_post(action: str , request: Request):
        if action == "import":
            return await run_in_threadpool(store.ingest , readcsv(await request.body()) , "CSV 文件导入")
        data = await body(request)
        if action == "network/pair":
            return store.makepair(data.get("label" , ""))
        if action == "network/revoke":
            return store.revoke(data.get("device_id"))
        if action == "network/bind":
            return store.queuebind(data.get("device_id") , data.get("uid") , data.get("name"))
        if action == "network/command":
            return store.queueoperation(data.get("device_id") , data.get("kind"))
        if action == "member/edit":
            store.edit(data.get("uid") , data)
            return {"message": "备注已保存"}
        handlers = {"device/connect": lambda: device.connect(data.get("port")) ,
                    "device/disconnect": device.disconnect , "device/members": device.members ,
                    "device/sync": lambda: device.sync(data.get("day" , today())) ,
                    "device/bind": lambda: device.bind(data.get("uid") , data.get("name")) , "device/clock": device.clock}
        if action not in handlers:
            raise Problem("接口不存在" , 404)
        return await run_in_threadpool(handlers[action])

    @app.get("/{asset:path}")
    def assets(asset: str):
        files = {"": ("index.html" , "text/html") , "app.js": ("app.js" , "text/javascript") , "network.js": ("network.js" , "text/javascript") ,
                 "style.css": ("style.css" , "text/css") , "favicon.svg": ("favicon.svg" , "image/svg+xml")}
        if asset not in files:
            raise Problem("页面不存在" , 404)
        filename , kind = files[asset]
        return Response((ROOT / "static" / filename).read_bytes() , media_type = kind)

    return app


def sol():
    parser = argparse.ArgumentParser(description = "NFC attendance network backend")
    parser.add_argument("--config" , default = str(ROOT / "network-config.json"))
    parser.add_argument("--db" , default = str(ROOT / "data" / "attendance.sqlite3"))
    args = parser.parse_args()
    path = Path(args.config)
    if not path.is_file():
        raise SystemExit("Copy network-config.example.json to network-config.json before starting")
    settings = Settings(**json.loads(path.read_text(encoding = "utf-8-sig")))
    logpath = Path(args.db).parent
    logpath.mkdir(parents = True , exist_ok = True)
    logging.basicConfig(level = logging.INFO , handlers = [RotatingFileHandler(logpath / "network.log" ,
                        maxBytes = 2 * 1024 * 1024 , backupCount = 3 , encoding = "utf-8")])
    app = create_app(args.db , settings)
    uvicorn.run(app , host = "127.0.0.1" , port = settings.port , proxy_headers = False , access_log = False)


if __name__ == "__main__":
    sol()
