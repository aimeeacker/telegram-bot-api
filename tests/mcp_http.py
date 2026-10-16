"""Exercise the built server over HTTP without Telegram credentials or bot calls."""

import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
MODERN = "2026-07-28"
LEGACY = "2025-11-25"
VERSION_KEY = "io.modelcontextprotocol/protocolVersion"
CAPABILITIES_KEY = "io.modelcontextprotocol/clientCapabilities"


class McpHttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mcp-http-")
        cls.addClassCleanup(cls.directory.cleanup)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            cls.port = sock.getsockname()[1]
        env = {key: value for key, value in os.environ.items() if not key.startswith("TELEGRAM_MCP_")}
        cls.log = open(Path(cls.directory.name) / "server.log", "w+")
        cls.addClassCleanup(cls.log.close)
        data_dir = Path(cls.directory.name) / "data"
        temp_dir = Path(cls.directory.name) / "temp"
        data_dir.mkdir(parents=True, exist_ok=True)
        temp_dir.mkdir(parents=True, exist_ok=True)
        cls.server = subprocess.Popen(
            [BINARY, "--api-id=1", "--api-hash=" + "0" * 32,
             "--http-ip-address=127.0.0.1", f"--http-port={cls.port}",
             "--preset-bots=123456:secret-token-123456",
             f"--dir={data_dir}", f"--temp-dir={temp_dir}"],
            env=env, stdout=cls.log, stderr=subprocess.STDOUT,
        )
        cls.addClassCleanup(cls.stop_server)
        deadline = time.monotonic() + 20
        while cls.server.poll() is None and time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", cls.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.1)
        cls.log.seek(0)
        raise RuntimeError("MCP test server did not start:\n" + cls.log.read())

    @classmethod
    def stop_server(cls):
        cls.server.terminate()
        try:
            cls.server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()

    def get_request(self, path):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            payload = response.read()
            return response.status, response.headers, payload
        finally:
            connection.close()

    def request(self, body, headers=None, path="/mcp"):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request("POST", path, body.encode(), {"Content-Type": "application/json", **(headers or {})})
            response = connection.getresponse()
            payload = response.read()
            return response.status, json.loads(payload) if payload else None
        finally:
            connection.close()

    def assert_error(self, body, code, **kwargs):
        status, payload = self.request(body, **kwargs)
        self.assertEqual(status, 400, payload)
        self.assertEqual(payload["error"]["code"], code, payload)

    def test_malformed_json(self):
        for body in [
            '{"jsonrpc":"2.0","id":1,"method":"ping"} {"x":"y"}',
            '{"jsonrpc":"2.0","id":1,"method":"ping","broken"}',
            '{"jsonrpc":"2.0","id":1,"method":"ping",}',
            '{"jsonrpc":"2.0","id":1,"method":"ping","broken":}',
            '{"jsonrpc":"2.0","id":1,"method":"ping"} {}',
            '{"jsonrpc":"2.0","id":1,"method":"ping","params":{"broken":}}',
            '{"jsonrpc":"2.0","id":1,"method":"ping',
            '{"jsonrpc":"2.0","id":1,"params":{"name":"send_message",'
            '"arguments":{"text":"hello"}},"method":"tools/call"} {"x":"y"}',
            "", " ",
        ]:
            with self.subTest(body=body):
                self.assert_error(body, -32700)

    def test_escaped_strings_are_decoded_once(self):
        for encoded in [r'"\u0031\""', r'"\n\""', r'"a\\"', r'"quote\"x"', r'"\\n"', r'"\u4f60\u597d"']:
            with self.subTest(encoded=encoded):
                body = r'{"jsonrpc":"\u0032.0","method":"p\u0069ng","id":' + encoded + "} \n"
                status, payload = self.request(body)
                self.assertEqual(status, 200, payload)
                self.assertEqual(payload["id"], json.loads(encoded))
                self.assertIn("result", payload)

    def test_optional_metadata_compatibility(self):
        for version in [None, LEGACY, MODERN]:
            for meta in [None, {"progressToken": 1}]:
                for method in ["ping", "server/discover", "tools/list", "tools/call"]:
                    with self.subTest(version=version, meta=meta, method=method):
                        params = {"name": "send_message", "arguments": {"text": "hello"}} if method == "tools/call" else {}
                        if meta is not None:
                            params["_meta"] = meta
                        headers = {"MCP-Protocol-Version": version} if version else {}
                        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
                        status, payload = self.request(body, headers)
                        self.assertEqual(status, 200, payload)
                        self.assertIn("result", payload)

    def test_modern_tool_request(self):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
            "name": "send_message", "arguments": {"text": "hello"},
            "_meta": {VERSION_KEY: MODERN, CAPABILITIES_KEY: {}},
        }})
        status, payload = self.request(body, {
            "MCP-Protocol-Version": MODERN, "Mcp-Method": "tools/call", "Mcp-Name": "send_message",
        })
        self.assertEqual(status, 200, payload)
        self.assertTrue(payload["result"]["isError"])  # No bot is configured in this test server.

    def test_supplied_metadata_and_headers_are_validated(self):
        for headers, params, code in [
            ({"Mcp-Method": "tools/list"}, {}, -32020),
            ({"MCP-Protocol-Version": "unsupported"}, {}, -32022),
            ({}, {"_meta": "bad"}, -32602),
            ({}, {"_meta": {CAPABILITIES_KEY: "bad"}}, -32602),
            ({"MCP-Protocol-Version": LEGACY}, {"_meta": {VERSION_KEY: MODERN}}, -32020),
        ]:
            with self.subTest(headers=headers, params=params):
                body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping", "params": params})
                self.assert_error(body, code, headers=headers)
        self.assert_error(
            '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"send_message"}}',
            -32020, headers={"Mcp-Name": "edit_message"},
        )

    def test_query_parameters_do_not_override_json_body(self):
        self.assert_error("{}", -32600, path="/mcp?jsonrpc=2.0&id=1&method=ping")
        status, payload = self.request(
            '{"jsonrpc":"2.0","id":1,"method":"ping"}',
            path="/mcp?jsonrpc=bad&id=999&method=tools/call",
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["id"], 1)

    def test_legacy_initialize_and_notifications(self):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": LEGACY}})
        status, payload = self.request(body)
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["result"]["protocolVersion"], LEGACY)
        self.assertEqual(self.request('{"jsonrpc":"2.0","method":"notifications/initialized"}'), (202, None))

    def test_tools_list_schema(self):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
        status, payload = self.request(body)
        self.assertEqual(status, 200, payload)
        names = [tool["name"] for tool in payload["result"]["tools"]]
        self.assertEqual(names, ["sendMessage", "editMessageText", "getUpdates", "setWebhook"])

    def test_mcp_get_updates(self):
        # Unexpected argument
        self.assert_error(
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                        "params": {"name": "getUpdates", "arguments": {"invalid": 123}}}),
            -32602,
        )
        # Timeout out of bounds
        self.assert_error(
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                        "params": {"name": "getUpdates", "arguments": {"timeout": 999}}}),
            -32602,
        )
        # Valid parameters (and alias poll_updates)
        for name in ["getUpdates", "poll_updates"]:
            body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": name, "arguments": {"limit": 10, "timeout": 20}}})
            status, payload = self.request(body)
            self.assertEqual(status, 200, payload)
            self.assertTrue(payload["result"]["isError"])
            self.assertIn("MCP Telegram bridge is not configured", payload["result"]["content"][0]["text"])

    def test_mcp_set_webhook_validation(self):
        # Domain name rejected
        for url in ["http://example.com/webhook", "https://api.mybot.org/hook", "http://localhost:8080/hook"]:
            body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": "setWebhook", "arguments": {"url": url}}})
            status, payload = self.request(body)
            self.assertEqual(status, 400, payload)
            self.assertEqual(payload["error"]["code"], -32602)
            self.assertIn("domain names are not allowed", payload["error"]["message"])

        # Public IP rejected
        for url in ["http://8.8.8.8/webhook", "https://1.1.1.1:8443/webhook"]:
            body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": "setWebhook", "arguments": {"url": url}}})
            status, payload = self.request(body)
            self.assertEqual(status, 400, payload)
            self.assertEqual(payload["error"]["code"], -32602)
            self.assertIn("private/reserved IP", payload["error"]["message"])

        # Private IP accepted (both http and https)
        for url in ["http://127.0.0.1:8080/webhook", "https://192.168.1.100:8443/hook", "http://10.0.0.1/hook"]:
            body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": "setWebhook", "arguments": {"url": url}}})
            status, payload = self.request(body)
            self.assertEqual(status, 200, payload)
            self.assertTrue(payload["result"]["isError"])
            self.assertIn("MCP Telegram bridge is not configured", payload["result"]["content"][0]["text"])

        # Empty URL (deleteWebhook) accepted
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                           "params": {"name": "setWebhook", "arguments": {"url": ""}}})
        status, payload = self.request(body)
        self.assertEqual(status, 200, payload)
        self.assertTrue(payload["result"]["isError"])
        self.assertIn("MCP Telegram bridge is not configured", payload["result"]["content"][0]["text"])

    def test_stats_and_root_redirect(self):
        status, headers, _ = self.get_request("/")
        self.assertEqual(status, 302)
        self.assertEqual(headers.get("Location"), "/stats")

        status, _, payload = self.get_request("/stats?format=json")
        self.assertEqual(status, 200)
        data = json.loads(payload)
        self.assertIn("version", data)

    def test_preset_bot_resolution(self):
        # 999999 is not preset -> rejected with 401 Unauthorized
        status_unauth, _, payload_unauth = self.get_request("/bot999999/close")
        self.assertEqual(status_unauth, 401)
        err = json.loads(payload_unauth)
        self.assertFalse(err["ok"])
        self.assertEqual(err["error_code"], 401)

        # 123456 is preset -> token resolved, reaches ClientManager, which responds with 400 bot already closed
        status, _, payload = self.get_request("/bot123456/close")
        self.assertEqual(status, 400)
        res = json.loads(payload)
        self.assertFalse(res["ok"])
        self.assertIn("already been closed", res["description"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
