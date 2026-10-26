#!/usr/bin/env python3
"""Provision only Cloudflare Access credentials on an explicitly matched tracker.

Default: read-only validation, followed by closing the local setup view.
Writes require --apply. Secrets are read from a local JSON file, never arguments.
Output contains only fixed status flags and known missing-field names. A save
with an uncertain response is never repeated; presence readback cannot establish
that the submitted token values match the stored values.
"""

from __future__ import annotations

import argparse
from http.cookies import SimpleCookie
import ipaddress
import json
from pathlib import Path
import re
import sys
import time
from urllib import error, parse, request


MAX_RESPONSE = 128 * 1024
MAX_INPUT = 1024 * 1024


class SafeFailure(Exception):
    """Contains a fixed diagnostic code only, never remote or secret content."""


class BoardFailure(SafeFailure):
    def __init__(self, status: int | None = None):
        super().__init__("board_request_failed")
        self.status = status


class NoRedirect(request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def selected_secret(document: object, selector: str) -> str:
    value = document
    for key in selector.split("."):
        if not key or not isinstance(value, dict) or key not in value:
            raise SafeFailure("secret_input_invalid")
        value = value[key]
    if not isinstance(value, str) or not 1 <= len(value) <= 128:
        raise SafeFailure("secret_input_invalid")
    if any(ord(char) < 33 or ord(char) > 126 for char in value):
        raise SafeFailure("secret_input_invalid")
    return value


def read_secrets(path: Path, id_key: str, secret_key: str) -> tuple[str, str]:
    try:
        with path.open("rb") as stream:
            contents = stream.read(MAX_INPUT + 1)
        if len(contents) > MAX_INPUT:
            raise SafeFailure("secret_input_invalid")
        document = json.loads(contents.decode("utf-8-sig"))
        return selected_secret(document, id_key), selected_secret(document, secret_key)
    except (OSError, ValueError, UnicodeError):
        raise SafeFailure("secret_input_invalid") from None


def board_origin(value: str) -> str:
    try:
        url = parse.urlsplit(value)
        address = ipaddress.IPv4Address(url.hostname or "")
        if (url.scheme != "http" or not address.is_private or address.is_loopback or
                address.is_unspecified or url.username is not None or url.password is not None or
                url.port not in (None, 80) or url.path not in ("", "/") or url.query or url.fragment):
            raise ValueError
        return "http://" + str(address)
    except ValueError:
        raise SafeFailure("board_address_invalid") from None


def validate_expected(endpoint: str, device_id: str) -> None:
    try:
        url = parse.urlsplit(endpoint)
        if (url.scheme != "https" or not url.hostname or not url.path or
                url.username is not None or url.password is not None or url.query or url.fragment or
                any(ord(char) < 33 or ord(char) > 126 for char in endpoint) or "\\" in endpoint or
                len(endpoint) > 256 or not device_id or len(device_id.encode("utf-8")) > 64):
            raise ValueError
        if url.port is not None and not 1 <= url.port <= 65535:
            raise ValueError
    except ValueError:
        raise SafeFailure("expected_target_invalid") from None


class Board:
    def __init__(self, origin: str, opener=None):
        self.origin = origin
        self.opener = opener or request.build_opener(request.ProxyHandler({}), NoRedirect())
        self.session = ""
        self.opened = False

    def exchange(self, path: str, body: dict | None = None, *, html=False):
        headers = {"Accept": "text/html" if html else "application/json"}
        if self.session:
            headers["Cookie"] = "tracker_session=" + self.session
        data = None
        if body is not None:
            data = json.dumps(body, separators=(",", ":")).encode("utf-8")
            headers.update({"Content-Type": "application/json", "Origin": self.origin,
                            "X-CSRF-Token": self.session})
        req = request.Request(self.origin + path, data=data, headers=headers)
        try:
            with self.opener.open(req, timeout=20) as response:
                content = response.read(MAX_RESPONSE + 1)
                if len(content) > MAX_RESPONSE:
                    raise BoardFailure()
                if html:
                    cookie = SimpleCookie()
                    cookie.load(response.headers.get("Set-Cookie", ""))
                    token = cookie.get("tracker_session")
                    if not token or not re.fullmatch(r"[0-9a-fA-F]{32}", token.value):
                        raise BoardFailure()
                    self.session = token.value
                    self.opened = True
                    return None
                result = json.loads(content)
                if not isinstance(result, dict):
                    raise BoardFailure()
                return result
        except error.HTTPError as exc:
            # Do not read/print an error body or exception containing credentials.
            status = exc.code
            exc.close()
            raise BoardFailure(status) from None
        except (OSError, ValueError, UnicodeError):
            raise BoardFailure() from None

    def config(self) -> dict:
        self.exchange("/", html=True)
        config = self.exchange("/api/config")
        if config.get("csrf_token") != self.session:
            raise BoardFailure()
        return config

    def close(self) -> bool:
        if not self.opened:
            return False
        try:
            reply = self.exchange("/api/close", {})
            return reply.get("ok") is True
        except BoardFailure as exc:
            # /api/close returns 409 only when shutdown is already scheduled.
            return exc.status == 409
        finally:
            self.opened = False


def missing_fields(config: dict) -> list[str]:
    fields = [name for name in ("endpoint", "device_id") if not config.get(name)]
    if config.get("network_mode") != "cellular_only" and not config.get("wifi_ssid"):
        fields.append("wifi_ssid")
    if config.get("network_mode") != "wifi_only" and not config.get("apn"):
        fields.append("apn")
    for name in ("access_client_id", "access_client_secret"):
        if config.get(name + "_set") is not True:
            fields.append(name)
    return fields


def target_matches(config: dict, args) -> bool:
    return (config.get("device_id") == args.expected_device_id and
            config.get("endpoint") == args.expected_endpoint)


def configure(args, *, opener=None, wait=time.sleep) -> dict:
    result = {"ok": False, "write_attempted": False, "save_acknowledged": False,
              "target_verified": False, "sessions_closed": False,
              "token_presence_confirmed": False, "token_values_verified": False,
              "outcome_unknown": False, "upload_ready": False, "missing_fields": []}
    try:
        origin = board_origin(args.board_url)
        validate_expected(args.expected_endpoint, args.expected_device_id)
        client_id, client_secret = read_secrets(args.secrets_file, args.client_id_key, args.client_secret_key)
    except SafeFailure as exc:
        result[str(exc)] = True
        return result
    initial = Board(origin, opener)
    save_acknowledged = False
    try:
        config = initial.config()
        result["target_verified"] = target_matches(config, args)
        if not result["target_verified"]:
            result["target_mismatch"] = True
            return result
        result["missing_fields"] = missing_fields(config)
        result["upload_ready"] = config.get("upload_ready") is True
        result["token_presence_confirmed"] = all(config.get(name + "_set") is True
            for name in ("access_client_id", "access_client_secret"))
        if not args.apply:
            result["ok"] = True
            return result
        result["write_attempted"] = True
        try:
            # Deliberately omit device_id, transport credentials, timings, PIN
            # and all other fields. The board merges these three fields only.
            reply = initial.exchange("/api/config", {"endpoint": args.expected_endpoint,
                "access_client_id": client_id, "access_client_secret": client_secret})
            save_acknowledged = reply.get("saved") is True
            result["save_acknowledged"] = save_acknowledged
            result["outcome_unknown"] = not save_acknowledged
        except BoardFailure as exc:
            result["outcome_unknown"] = exc.status is None or exc.status >= 500
            if not result["outcome_unknown"]:
                result["save_rejected"] = True
                return result
    except BoardFailure:
        result["board_read_failed"] = True
        return result
    finally:
        # Acknowledged /api/config already schedules session closure. Every
        # other path, including mismatch and uncertain writes, explicitly exits.
        result["sessions_closed"] = save_acknowledged or initial.close()
        if not result["sessions_closed"]:
            result["ok"] = False
    wait(1.0)  # Allow the board's scheduled close/reconfigure to finish.
    verification = Board(origin, opener)
    try:
        config = verification.config()
        result["target_verified"] = target_matches(config, args)
        result["missing_fields"] = missing_fields(config)
        result["upload_ready"] = config.get("upload_ready") is True
        result["token_presence_confirmed"] = all(config.get(name + "_set") is True
            for name in ("access_client_id", "access_client_secret"))
        result["ok"] = (save_acknowledged and result["target_verified"] and
                        result["token_presence_confirmed"])
    except BoardFailure:
        result["verification_failed"] = True
    finally:
        closed = verification.close()
        result["sessions_closed"] = result["sessions_closed"] and closed
        result["ok"] = result["ok"] and result["sessions_closed"]
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board-url", default="http://192.168.88.68")
    parser.add_argument("--secrets-file", type=Path, required=True)
    parser.add_argument("--client-id-key", default="client_id", help="Dot-separated JSON selector; never a token value")
    parser.add_argument("--client-secret-key", default="client_secret", help="Dot-separated JSON selector; never a token value")
    parser.add_argument("--expected-device-id", required=True)
    parser.add_argument("--expected-endpoint", required=True)
    parser.add_argument("--apply", action="store_true", help="Write credentials once after identity checks")
    args = parser.parse_args(argv)
    try:
        result = configure(args)
    except KeyboardInterrupt:
        result = {"ok": False, "interrupted": True}
    except Exception:
        # No traceback or exception text: dependencies may include request data.
        result = {"ok": False, "unexpected_failure": True}
    print(json.dumps(result, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
