import copy
from datetime import datetime , timezone
import json
from pathlib import Path
import secrets
import sys
import tempfile
import time
import unittest

from cryptography.hazmat.primitives.asymmetric import rsa
import jwt
from fastapi.testclient import TestClient

sys.path.insert(0 , str(Path(__file__).resolve().parents[1]))
from netstore import NetStore , eventid , OPERATIONS
from network_server import AccessVerifier , Settings , create_app
from validation import Problem

IDENT = "s3-aabbccddeeff"
SECRET = "a" * 64
ROW = {"time": "2026-11-07 05:56:24" , "uid": "04-11-22-33" , "name": "student" , "status": "SAVED"}


class NetworkTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.path = Path(self.folder.name) / "attendance.sqlite3"
        self.app = create_app(self.path)
        self.client = TestClient(self.app , base_url = "http://127.0.0.1:18080")
        self.db = self.app.state.store
        code = self.db.makepair("test")["code"]
        self.pairdata = {"code": code , "device_id": IDENT , "secret": SECRET}
        self.db.pair(self.pairdata)
        self.headers = {"Host": "device-api.example.com" , "X-Device-ID": IDENT , "Authorization": "Bearer " + SECRET}
        self.row = {**ROW , "event_id": eventid(IDENT , ROW["time"][:10] , ROW["uid"])}

    def tearDown(self):
        self.client.close()
        self.folder.cleanup()

    def post(self , endpoint , data):
        return self.client.post("/device/v1/" + endpoint , json = data , headers = self.headers)

    def test_offline_replay_and_restart(self):
        self.assertEqual(self.post("events" , {"events": [self.row]}).json()["added"] , 1)
        for i in range(3):
            self.assertEqual(self.post("events" , {"events": [self.row]}).json()["acked"] , [self.row["event_id"]])
        self.assertEqual(NetStore(self.path).overview("2026-11-07")["count"] , 1)

    def test_usb_history_association(self):
        self.db.ingest([ROW] , "USB")
        self.assertEqual(self.post("events" , {"events": [self.row]}).json()["added"] , 0)
        with self.db.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM net_events").fetchone()[0] , 1)

    def test_conflict_rolls_back_whole_batch(self):
        self.post("events" , {"events": [self.row]})
        other = {**self.row , "uid": "01-02-03-04"}
        other["event_id"] = eventid(IDENT , ROW["time"][:10] , other["uid"])
        bad = {**self.row , "name": "conflict"}
        response = self.post("events" , {"events": [other , bad]})
        self.assertEqual(response.status_code , 409)
        self.assertEqual(self.db.overview("2026-11-07")["count"] , 1)

    def test_forged_event_id_and_bad_row(self):
        for changes in ({"event_id": "0" * 64} , {"status": "FAIL"} , {"time": "2026-11-07 25:00:00"} , {"name": "=1"}):
            self.assertEqual(self.post("events" , {"events": [{**self.row , **changes}]}).status_code , 400)

    def test_pair_retry_and_single_use(self):
        self.assertTrue(self.db.pair(self.pairdata)["paired"])
        with self.assertRaises(Problem):
            self.db.pair({**self.pairdata , "secret": "b" * 64})
        with self.assertRaises(Problem):
            self.db.pair({**self.pairdata , "code": "0" * 16})

    def test_expired_pair(self):
        code = self.db.makepair("expired")["code"]
        with self.db.connect() as db:
            db.execute("UPDATE net_pairs SET expires=0")
        with self.assertRaises(Problem):
            self.db.pair({**self.pairdata , "code": code})

    def test_device_secret_and_revocation(self):
        bad = {**self.headers , "Authorization": "Bearer " + "b" * 64}
        self.assertEqual(self.client.post("/device/v1/heartbeat" , json = {} , headers = bad).status_code , 401)
        self.assertEqual(self.post("heartbeat" , {"version": "0.9.0"}).status_code , 200)
        self.db.revoke(IDENT)
        self.assertEqual(self.post("heartbeat" , {}).status_code , 401)
        with self.assertRaises(Problem):
            self.db.pair(self.pairdata)

    def test_host_and_route_separation(self):
        for path in ("/" , "/api/backup" , "/api/bootstrap" , "/api/network"):
            self.assertEqual(self.client.get(path , headers = self.headers).status_code , 403)
        self.assertEqual(self.client.post("/device/v1/heartbeat" , json = {} , headers = {**self.headers , "Host": "checkin.example.com"}).status_code , 404)
        self.assertEqual(self.client.get("/api/bootstrap" , headers = {"CF-Ray": "external"}).status_code , 403)
        self.assertEqual(self.client.get("/api/bootstrap" , headers = {"Host": "checkin.example.com"}).status_code , 401)

    def test_csrf_and_size(self):
        self.assertEqual(self.client.post("/api/network/pair" , json = {}).status_code , 403)
        token = self.client.get("/api/bootstrap").json()["token"]
        response = self.client.post("/api/network/pair" , json = {} , headers = {"X-Admin-Token": token , "Origin": "https://evil.invalid"})
        self.assertEqual(response.status_code , 403)
        response = self.client.post("/device/v1/events" , content = b"x" * 140000 , headers = self.headers)
        self.assertEqual(response.status_code , 413)

    def test_pending_registration_and_ack_retry(self):
        command = self.db.queuebind(IDENT , ROW["uid"] , ROW["name"])
        self.assertEqual(self.db.members() , [])
        self.assertEqual(self.db.commands(IDENT)["commands"][0]["id"] , command["id"])
        ack = {"id": command["id"] , "ok": True , "detail": "SD_READBACK_OK"}
        self.assertEqual(self.post("command-ack" , ack).status_code , 200)
        self.assertEqual(self.post("command-ack" , ack).status_code , 200)
        self.assertEqual(self.db.commands(IDENT)["commands"] , [])
        self.assertEqual(self.db.members()[0]["on_device"] , 1)
        self.assertEqual(self.post("command-ack" , {**ack , "ok": False}).status_code , 409)

    def test_failed_registration_not_reported_successful(self):
        command = self.db.queuebind(IDENT , ROW["uid"] , ROW["name"])
        self.post("command-ack" , {"id": command["id"] , "ok": False , "detail": "SD_ERROR"})
        self.assertEqual(self.db.members() , [])

    def test_roster_and_heartbeat(self):
        self.assertEqual(self.post("roster" , {"members": [{"uid": ROW["uid"] , "name": ROW["name"]}]}).json()["count"] , 1)
        self.post("heartbeat" , {"version": "0.9.0" , "pending": 1 , "secret": SECRET})
        result = self.db.devices()["devices"][0]
        self.assertTrue(result["online"])
        self.assertNotIn("secret" , json.dumps(result))

    def enable_operations(self):
        self.assertEqual(self.post("heartbeat" , {"version": "0.10.0" , "capabilities": list(OPERATIONS) ,
                         "oled": "INITIALIZED" , "registry": "READY"}).status_code , 200)

    def queue_operation(self , kind):
        token = self.client.get("/api/bootstrap").json()["token"]
        return self.client.post("/api/network/command" , json = {"device_id": IDENT , "kind": kind} , headers = {"X-Admin-Token": token})

    def test_legacy_firmware_keeps_bind_but_rejects_new_operations(self):
        self.post("heartbeat" , {"version": "0.9.0"})
        self.assertEqual(self.queue_operation("clock").status_code , 409)
        self.db.queuebind(IDENT , ROW["uid"] , ROW["name"])
        self.assertEqual(self.db.commands(IDENT)["commands"][0]["kind"] , "bind")

    def test_remote_operations_offline_dedup_claim_restart_and_confirm(self):
        self.enable_operations()
        with self.db.connect() as db:
            db.execute("UPDATE net_devices SET seen=0")
        self.assertFalse(self.db.devices()["devices"][0]["online"])
        for kind , expected in OPERATIONS.items():
            response = self.queue_operation(kind)
            self.assertEqual(response.status_code , 200)
            key = response.json()["id"]
            self.assertEqual(response.json()["state"] , "pending")
            self.assertEqual(self.queue_operation(kind).json()["id"] , key)
            command = self.db.commands(IDENT)["commands"][0]
            self.assertEqual(command , {"id": key , "kind": kind})
            row = next(c for c in self.db.devices()["commands"] if c["id"] == key)
            self.assertEqual(row["state"] , "running")
            self.assertIsNotNone(row["started_at"])
            reopened = NetStore(self.path)
            self.assertEqual(reopened.commands(IDENT)["commands"][0]["id"] , key)
            self.assertEqual(self.post("command-ack" , {"id": key , "ok": True , "detail": "SCHEDULED"}).status_code , 409)
            self.assertEqual(self.post("command-ack" , {"id": key , "ok": True , "detail": expected}).status_code , 200)
            self.assertEqual(self.post("command-ack" , {"id": key , "ok": True , "detail": expected}).status_code , 200)
            self.assertEqual(self.db.commands(IDENT)["commands"] , [])
        self.assertEqual(self.db.members() , [])

    def test_failed_remote_operation_and_ack_conflict(self):
        self.enable_operations()
        command = self.queue_operation("rescan").json()
        ack = {"id": command["id"] , "ok": False , "detail": "SD_HISTORY_INVALID"}
        self.assertEqual(self.post("command-ack" , ack).status_code , 200)
        row = self.db.devices()["commands"][0]
        self.assertEqual(row["state"] , "failed")
        self.assertEqual(self.post("command-ack" , {**ack , "ok": True , "detail": "HISTORY_SCAN_ACKED"}).status_code , 409)
        self.assertEqual(self.db.overview(ROW["time"][:10])["count"] , 0)

    def test_firmware_rollback_does_not_deliver_unsupported_commands(self):
        self.enable_operations()
        key = self.queue_operation("clock").json()["id"]
        self.assertEqual(self.db.commands(IDENT)["commands"][0]["id"] , key)
        self.post("heartbeat" , {"version": "0.9.0"})
        self.assertEqual(self.db.commands(IDENT)["commands"] , [])
        self.db.queuebind(IDENT , ROW["uid"] , ROW["name"])
        self.assertEqual(self.db.commands(IDENT)["commands"][0]["kind"] , "bind")

    def test_operation_validation_and_permissions(self):
        self.enable_operations()
        for kind in ("erase" , None , ["clock"]):
            self.assertEqual(self.queue_operation(kind).status_code , 400)
        self.assertEqual(self.client.post("/api/network/command" , json = {"device_id": IDENT , "kind": "clock"}).status_code , 403)
        self.assertEqual(self.client.post("/api/network/command" , json = {"device_id": IDENT , "kind": "clock"} , headers = self.headers).status_code , 403)
        self.db.revoke(IDENT)
        self.assertEqual(self.queue_operation("clock").status_code , 404)

    def test_additive_command_migration_retains_existing_data(self):
        self.db.ingest([ROW] , "USB")
        command = self.db.queuebind(IDENT , ROW["uid"] , ROW["name"])
        with self.db.connect() as db:
            db.execute("ALTER TABLE net_commands DROP COLUMN started_at")
        reopened = NetStore(self.path)
        self.assertEqual(reopened.commands(IDENT)["commands"][0]["id"] , command["id"])
        self.assertEqual(reopened.overview(ROW["time"][:10])["count"] , 1)
        self.assertEqual(NetStore(self.path).commands(IDENT)["commands"][0]["id"] , command["id"])

    def test_heartbeat_capabilities_are_filtered(self):
        for invalid in ("clock" , None , {}):
            self.post("heartbeat" , {"capabilities": invalid})
            self.assertEqual(self.db.devices()["devices"][0]["capabilities"] , [])
        self.post("heartbeat" , {"capabilities": ["clock" , "erase" , "clock"] , "secret": SECRET})
        result = self.db.devices()["devices"][0]
        self.assertEqual(result["capabilities"] , ["clock"])
        self.assertNotIn(SECRET , json.dumps(result))


class AccessTests(unittest.TestCase):
    def setUp(self):
        self.key = rsa.generate_private_key(public_exponent = 65537 , key_size = 2048)
        self.settings = Settings(access_issuer = "https://test.cloudflareaccess.com" , access_audience = "test-aud" , local_admin = False)
        self.verifier = AccessVerifier(self.settings)
        public = self.key.public_key()
        class Keys:
            def get_signing_key_from_jwt(self , token):
                return type("Key" , () , {"key": public})()
        self.verifier.client = Keys()
        self.claims = {"iss": self.settings.access_issuer , "aud": "test-aud" , "sub": "admin" ,
                       "email": "admin@example.com" , "type": "app" , "iat": int(time.time()) , "exp": int(time.time()) + 60}

    def test_valid_access_jwt(self):
        self.verifier.verify(jwt.encode(self.claims , self.key , algorithm = "RS256"))

    def test_reject_expired_audience_issuer_identity_and_type(self):
        for changes in ({"exp": int(time.time()) - 30} , {"aud": "other"} , {"iss": "https://other.invalid"} ,
                        {"email": "other@example.com"} , {"type": "service"}):
            with self.assertRaises(Problem):
                self.verifier.verify(jwt.encode({**self.claims , **changes} , self.key , algorithm = "RS256"))

    def test_reject_bad_signature(self):
        other = rsa.generate_private_key(public_exponent = 65537 , key_size = 2048)
        with self.assertRaises(Problem):
            self.verifier.verify(jwt.encode(self.claims , other , algorithm = "RS256"))

    def test_production_config_fails_closed(self):
        with self.assertRaises(ValueError):
            Settings(local_admin = False).validate()


if __name__ == "__main__":
    unittest.main()
