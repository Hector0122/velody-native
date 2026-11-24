"""In-memory stand-in for velody's cloud Worker (velody/cloud-worker), for the C++ cloud tests.

Same API and rules as the real one: bearer auth, upload only changed files, commit checks storage,
last-write-wins metadata (409 with the stored version when it isn't newer). "R2" is served by this
same server behind presigned-looking URLs whose query must come back byte for byte.

Prints the port on the first stdout line. Test hooks: POST /_fail?n=N makes the next N API calls
answer 503; GET /_state dumps the store.
"""
import hashlib, json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

TOKEN = sys.argv[1] if len(sys.argv) > 1 else "secret"
SIG = "sig=a%2Fb%3Dc"  # encoded chars: the client must not re-encode the presigned query

lock = threading.Lock()
songs = {}      # id -> {name, files: {name: {size, sha256}}, pending, meta, updatedAt}
blobs = {}      # (id, name) -> bytes
fail_next = [0]


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, obj=None, raw=None):
        body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def r2_url(self, sid, name):
        host = self.headers.get("Host")
        return f"http://{host}/r2/{sid}/{name}?{SIG}"

    def handle_any(self, method):
        parts = urlsplit(self.path)
        path = parts.path

        if path == "/_fail":
            fail_next[0] = int(parse_qs(parts.query)["n"][0])
            return self.send(200, {})
        if path == "/_state":
            with lock:
                return self.send(200, {k: {**v, "pending": None} for k, v in songs.items()})

        if path.startswith("/r2/"):
            if parts.query != SIG:
                return self.send(403, {"error": "bad signature"})
            _, _, sid, name = path.split("/", 3)
            if method == "PUT":
                with lock:
                    blobs[(sid, name)] = self.body()
                return self.send(200, raw=b"")
            data = blobs.get((sid, name))
            return self.send(200, raw=data) if data is not None else self.send(404, {"error": "no blob"})

        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            return self.send(401, {"error": "Unauthorized"})
        if fail_next[0] > 0:
            fail_next[0] -= 1
            return self.send(503, {"error": "busy"})
        if not path.startswith("/v1/"):
            return self.send(404, {"error": "not found"})
        segs = path[len("/v1/"):].split("/")

        with lock:
            if segs == ["health"]:
                return self.send(200, {"ok": True})
            if segs == ["songs"] and method == "GET":
                out = [{"id": k, "name": v["name"],
                        "files": [{"name": n, **f} for n, f in sorted(v["files"].items())],
                        "metaUpdatedAt": v.get("updatedAt"), "updatedAt": 1}
                       for k, v in songs.items() if v["files"]]
                return self.send(200, {"songs": out})
            if len(segs) >= 2 and segs[0] == "songs":
                sid = segs[1]
                action = segs[2] if len(segs) > 2 else None
                song = songs.get(sid)
                if action == "upload" and method == "POST":
                    req = json.loads(self.body())
                    s = songs.setdefault(sid, {"name": req["name"], "files": {}, "meta": None, "updatedAt": None})
                    s["name"] = req["name"]
                    s["pending"] = {f["name"]: {"size": f["size"], "sha256": f["sha256"]} for f in req["files"]}
                    ups = [{"name": n, "url": self.r2_url(sid, n)} for n, f in s["pending"].items() if s["files"].get(n) != f]
                    return self.send(200, {"uploads": ups, "expiresAt": 0})
                if song is None:
                    return self.send(404, {"error": "Song not found"})
                if action == "commit" and method == "POST":
                    pending = song.get("pending") or song["files"]
                    missing = [n for n, f in pending.items()
                               if (sid, n) not in blobs or len(blobs[(sid, n)]) != f["size"]]
                    if missing:
                        return self.send(409, {"error": "Some files are missing in storage", "missing": missing})
                    song["files"] = pending
                    files = [{"name": n, **f} for n, f in sorted(pending.items())]
                    return self.send(200, {"song": {"id": sid, "name": song["name"], "files": files,
                                                    "metaUpdatedAt": song["updatedAt"], "updatedAt": 1}})
                if action == "download" and method == "GET":
                    files = [{"name": n, **f, "url": self.r2_url(sid, n)} for n, f in sorted(song["files"].items())]
                    return self.send(200, {"files": files, "expiresAt": 0})
                if action == "meta" and method == "GET":
                    if song["meta"] is None:
                        return self.send(404, {"error": "No metadata"})
                    return self.send(200, {"meta": song["meta"], "updatedAt": song["updatedAt"]})
                if action == "meta" and method == "PUT":
                    req = json.loads(self.body())
                    if song["updatedAt"] is not None and song["updatedAt"] >= req["updatedAt"]:
                        return self.send(409, {"error": "A newer version is stored",
                                               "meta": song["meta"], "updatedAt": song["updatedAt"]})
                    song["meta"], song["updatedAt"] = req["meta"], req["updatedAt"]
                    return self.send(200, {"updatedAt": req["updatedAt"]})
                if action is None and method == "DELETE":
                    songs.pop(sid)
                    for k in [k for k in blobs if k[0] == sid]:
                        blobs.pop(k)
                    return self.send(200, {"ok": True})
        return self.send(404, {"error": "not found"})

    def do_GET(self): self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_PUT(self): self.handle_any("PUT")
    def do_DELETE(self): self.handle_any("DELETE")


srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
