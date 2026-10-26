"""Offline provisioning tests. All HTTP exchanges use an in-process fake."""

import argparse
import contextlib
import io
import json
from pathlib import Path
import unittest
from unittest import mock
from urllib import error
import uuid

import configure_tracker_access as provision


CLIENT_ID = "fictional-new-client-id"
CLIENT_SECRET = "fictional-new-client-secret"
ENDPOINT = "https://tracker.davidmolnar.dev/ingest"


class Response:
    def __init__(self, body, headers=None):
        self.content = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.headers = headers or {}

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def read(self, size):
        return self.content[:size]


class FakeBoard:
    def __init__(self):
        self.config = {"device_id": "pablo", "endpoint": ENDPOINT, "network_mode": "wifi_only",
            "wifi_ssid": "home", "wifi_password": "unchanged-wifi-password", "apn": "",
            "access_client_id": "", "access_client_secret": "", "sim_pin": "0042",
            "setup_password": "unchanged-ap-password", "web_password": "",
            "active_interval_s": 30, "grace_period_s": 600, "parked_interval_s": 1800,
            "bench_mode": True}
        self.writes = []
        self.session = ""
        self.sessions = 0
        self.close_requests = 0
        self.editing = False
        self.save_timeout = False
        self.commit_on_timeout = False
        self.reject_save = False
        self.close_timeout = False

    def open(self, req, timeout):
        assert timeout == 20
        path = req.full_url.removeprefix("http://192.168.88.68")
        if req.get_method() == "GET" and path == "/":
            self.sessions += 1
            self.session = f"{self.sessions:032x}"
            self.editing = True
            return Response(b"<html>setup</html>", {"Set-Cookie": f"tracker_session={self.session}; HttpOnly; Path=/"})
        assert req.get_header("Cookie") == "tracker_session=" + self.session
        if req.get_method() == "GET" and path == "/api/config":
            redacted = {key: value for key, value in self.config.items()
                        if key not in ("wifi_password", "access_client_id", "access_client_secret", "sim_pin", "setup_password", "web_password")}
            for name in ("access_client_id", "access_client_secret"):
                redacted[name + "_set"] = bool(self.config[name])
            redacted["csrf_token"] = self.session
            redacted["upload_ready"] = not provision.missing_fields(redacted)
            return Response(redacted)
        assert req.get_method() == "POST"
        assert req.get_header("Origin") == "http://192.168.88.68"
        assert req.get_header("X-csrf-token") == self.session
        body = json.loads(req.data)
        if path == "/api/close":
            assert body == {}
            self.close_requests += 1
            if self.close_timeout:
                raise TimeoutError(CLIENT_SECRET)
            self.editing = False
            return Response({"ok": True})
        assert path == "/api/config"
        self.writes.append(body)
        if self.reject_save:
            raise error.HTTPError(req.full_url, 422, CLIENT_SECRET, {}, io.BytesIO(CLIENT_SECRET.encode()))
        if not self.save_timeout or self.commit_on_timeout:
            self.config.update(body)
            self.editing = False  # Firmware's successful save schedules closure.
        if self.save_timeout:
            raise TimeoutError(CLIENT_SECRET)
        return Response({"saved": True, "upload_ready": True, "ignored_diagnostic": CLIENT_SECRET})


class AccessProvisioningTests(unittest.TestCase):
    def setUp(self):
        self.private_root = Path(__file__).resolve().parent / "provisioning-private"
        self.root = self.private_root / ("access-test-" + uuid.uuid4().hex)
        self.root.mkdir(parents=True)
        self.path = self.root / "private.json"
        self.path.write_text(json.dumps({"cloudflare": {"client_id": CLIENT_ID, "client_secret": CLIENT_SECRET}}), encoding="utf-8")
        self.args = argparse.Namespace(board_url="http://192.168.88.68", secrets_file=self.path,
            client_id_key="cloudflare.client_id", client_secret_key="cloudflare.client_secret",
            expected_device_id="pablo", expected_endpoint=ENDPOINT, apply=False)
        self.board = FakeBoard()

    def tearDown(self):
        self.assertEqual(self.root.resolve().parent, self.private_root.resolve())
        self.assertTrue(self.root.name.startswith("access-test-"))
        self.path.unlink(missing_ok=True)
        self.root.rmdir()

    def execute(self):
        return provision.configure(self.args, opener=self.board, wait=lambda _: None)

    def assert_no_secret(self, output):
        self.assertNotIn(CLIENT_ID, output)
        self.assertNotIn(CLIENT_SECRET, output)
        self.assertNotIn("unchanged-wifi-password", output)
        self.assertNotIn("unchanged-ap-password", output)

    def test_default_read_only_closes_editor(self):
        result = self.execute()
        self.assertTrue(result["ok"])
        self.assertFalse(result["write_attempted"])
        self.assertEqual(self.board.writes, [])
        self.assertEqual(self.board.close_requests, 1)
        self.assertFalse(self.board.editing)
        self.assertEqual(result["missing_fields"], ["access_client_id", "access_client_secret"])
        self.assert_no_secret(json.dumps(result))

    def test_apply_changes_only_three_allowed_fields_and_closes_readback(self):
        before = dict(self.board.config)
        self.args.apply = True
        result = self.execute()
        self.assertTrue(result["ok"])
        self.assertTrue(result["save_acknowledged"])
        self.assertTrue(result["sessions_closed"])
        self.assertTrue(result["token_presence_confirmed"])
        self.assertTrue(result["upload_ready"])
        self.assertFalse(result["token_values_verified"])
        self.assertEqual(self.board.writes, [{"endpoint": ENDPOINT,
            "access_client_id": CLIENT_ID, "access_client_secret": CLIENT_SECRET}])
        for key, value in before.items():
            if key not in ("access_client_id", "access_client_secret"):
                self.assertEqual(self.board.config[key], value)
        self.assertEqual(self.board.config["device_id"], "pablo")
        self.assertEqual(self.board.sessions, 2)
        self.assertEqual(self.board.close_requests, 1)
        self.assertFalse(self.board.editing)
        self.assert_no_secret(json.dumps(result))

    def test_mismatched_device_or_endpoint_prevents_write_and_closes(self):
        self.args.apply = True
        for field, value in (("device_id", "other-car"), ("endpoint", "https://other.example/ingest")):
            with self.subTest(field=field):
                self.board = FakeBoard()
                self.board.config[field] = value
                result = self.execute()
                self.assertFalse(result["ok"])
                self.assertTrue(result["target_mismatch"])
                self.assertFalse(result["write_attempted"])
                self.assertEqual(self.board.writes, [])
                self.assertFalse(self.board.editing)

    def test_unknown_save_is_not_repeated_or_claimed_equal(self):
        self.args.apply = True
        for committed in (False, True):
            with self.subTest(committed=committed):
                self.board = FakeBoard()
                self.board.save_timeout = True
                self.board.commit_on_timeout = committed
                result = self.execute()
                self.assertFalse(result["ok"])
                self.assertTrue(result["outcome_unknown"])
                self.assertFalse(result["save_acknowledged"])
                self.assertFalse(result["token_values_verified"])
                self.assertEqual(result["token_presence_confirmed"], committed)
                self.assertEqual(len(self.board.writes), 1)
                self.assertEqual(self.board.close_requests, 2)
                self.assertFalse(self.board.editing)
                self.assert_no_secret(json.dumps(result))

    def test_existing_tokens_may_be_replaced_explicitly(self):
        self.args.apply = True
        self.board.config.update(access_client_id="old-client", access_client_secret="old-secret")
        result = self.execute()
        self.assertTrue(result["ok"])
        self.assertEqual(len(self.board.writes), 1)
        self.assertEqual(self.board.config["access_client_secret"], CLIENT_SECRET)

    def test_preexisting_tokens_do_not_resolve_uncertain_replacement(self):
        self.args.apply = True
        self.board.save_timeout = True
        self.board.config.update(access_client_id="old-client", access_client_secret="old-secret")
        result = self.execute()
        self.assertFalse(result["ok"])
        self.assertTrue(result["outcome_unknown"])
        self.assertTrue(result["token_presence_confirmed"])
        self.assertFalse(result["token_values_verified"])
        self.assertEqual(self.board.config["access_client_secret"], "old-secret")
        self.assertEqual(len(self.board.writes), 1)
        self.assertFalse(self.board.editing)

    def test_other_missing_fields_are_reported_without_changing_them(self):
        self.args.apply = True
        self.board.config["wifi_ssid"] = ""
        result = self.execute()
        self.assertTrue(result["ok"])
        self.assertFalse(result["upload_ready"])
        self.assertEqual(result["missing_fields"], ["wifi_ssid"])
        self.assertEqual(self.board.config["wifi_ssid"], "")

    def test_close_failure_is_not_reported_as_success(self):
        self.board.close_timeout = True
        result = self.execute()
        self.assertFalse(result["ok"])
        self.assertFalse(result["sessions_closed"])
        self.assert_no_secret(json.dumps(result))

    def test_bad_secret_file_is_rejected_before_board_request(self):
        self.path.write_text("not json " + CLIENT_SECRET, encoding="utf-8")
        result = self.execute()
        self.assertFalse(result["ok"])
        self.assertTrue(result["secret_input_invalid"])
        self.assertEqual(self.board.sessions, 0)
        self.assert_no_secret(json.dumps(result))

    def test_cli_redacts_remote_errors(self):
        self.board.reject_save = True
        argv = ["--secrets-file", str(self.path), "--client-id-key", "cloudflare.client_id",
            "--client-secret-key", "cloudflare.client_secret", "--expected-device-id", "pablo",
            "--expected-endpoint", ENDPOINT, "--apply"]
        output, errors = io.StringIO(), io.StringIO()
        with mock.patch.object(provision.request, "build_opener", return_value=self.board), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = provision.main(argv)
        self.assertEqual(code, 1)
        self.assertTrue(json.loads(output.getvalue())["save_rejected"])
        self.assertFalse(self.board.editing)
        self.assertEqual(errors.getvalue(), "")
        self.assert_no_secret(output.getvalue())
        self.assertNotIn("Traceback", output.getvalue())


if __name__ == "__main__":
    unittest.main()
