import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


BINARY = None
LIMIT = 1024 * 1024


def release(tag="v1.2.3", **fields):
    return json.dumps(dict(tag_name=tag, draft=False, prerelease=False, **fields)).encode()


def config(*names):
    return "".join(f"[{name}]\nsource = github\nrepo = owner/{name}\n" for name in names)


class Fixture:
    def __init__(self):
        self.routes = {}
        self.requests = []
        self.active = 0
        self.peak = 0
        self.lock = threading.Condition()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                with fixture.lock:
                    fixture.requests.append((self.path, dict(self.headers)))
                    fixture.active += 1
                    fixture.peak = max(fixture.peak, fixture.active)
                    fixture.lock.notify_all()
                try:
                    route = fixture.routes.get(self.path, (404, b"not found", {}))
                    if callable(route):
                        route = route()
                    status, body, headers = route
                    self.send_response(status)
                    chunks = body if isinstance(body, list) else [body]
                    self.send_header("Content-Length", str(sum(map(len, chunks))))
                    self.send_header("Content-Type", "application/json")
                    for key, value in headers.items():
                        self.send_header(key, value)
                    self.end_headers()
                    for chunk in chunks:
                        self.wfile.write(chunk)
                        self.wfile.flush()
                        if len(chunks) > 1:
                            time.sleep(0.001)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    with fixture.lock:
                        fixture.active -= 1
                        fixture.lock.notify_all()

        class Server(ThreadingHTTPServer):
            request_queue_size = 128

        self.server = Server(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def add(self, name, body=None, status=200, headers=None, delay=0):
        response = (status, release() if body is None else body, headers or {})
        if delay:
            def delayed():
                time.sleep(delay)
                return response
            response_route = delayed
        else:
            response_route = response
        self.routes[f"/repos/owner/{name}/releases/latest"] = response_route

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class NvcTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "versions.ini"

    def run_nvc(self, text=None, args=(), env=None, use_config=True):
        if text is not None:
            self.path.write_bytes(text if isinstance(text, bytes) else text.encode())
        environment = os.environ.copy()
        for key in tuple(environment):
            if key.lower().endswith("_proxy") or key in (
                "GITHUB_TOKEN", "NVC_TEST_URL", "NVC_TEST_TIMEOUT_MS"
            ):
                environment.pop(key)
        environment.update(NVC_TEST_URL=self.fixture.url, NVC_TEST_TIMEOUT_MS="2000")
        environment.update(env or {})
        command = [BINARY]
        if use_config:
            command += ["-c", str(self.path)]
        return subprocess.run(command + list(args), env=environment, capture_output=True, timeout=10)

    def assert_failure(self, result, name=None, pattern=None):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertTrue(result.stderr.strip())
        if name is not None:
            self.assertIn(name.encode(), result.stderr)
        if pattern is not None:
            self.assertRegex(result.stderr.decode(errors="replace").lower(), pattern)

    def assert_no_network(self):
        self.assertEqual(self.fixture.requests, [])

    def test_valid_config_and_exact_versions(self):
        self.fixture.add("first", release("v01.2+build-β"))
        self.fixture.add("Second", release("release candidate"))
        text = "# comment\n; comment\n\n[ first ]\n source = github \n repo = owner/first \n\n[Second]\nsource=github\nrepo=owner/Second\n"
        result = self.run_nvc(text)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "first\tv01.2+build-β\nSecond\trelease candidate\n".encode())
        self.assertEqual(result.stderr, b"")
        self.assertCountEqual([r[0] for r in self.fixture.requests], list(self.fixture.routes))
        for _, headers in self.fixture.requests:
            self.assertTrue(headers.get("User-Agent"))
            self.assertNotIn("Authorization", headers)

    def test_selection_uses_config_order(self):
        for name in ("first", "second", "third"):
            self.fixture.add(name)
        result = self.run_nvc(config("first", "second", "third"), ("third", "first"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"first\tv1.2.3\nthird\tv1.2.3\n")
        self.assertCountEqual([r[0] for r in self.fixture.requests], [
            "/repos/owner/first/releases/latest", "/repos/owner/third/releases/latest"
        ])

    def test_large_config_lookup_and_duplicate_selection(self):
        names = [f"p{i:05d}" for i in reversed(range(12000))]
        selected = [names[0], names[len(names) // 2], names[-1]]
        for name in selected:
            self.fixture.add(name)
        result = self.run_nvc(config(*names), tuple(reversed(selected)) * 1000)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "".join(f"{name}\tv1.2.3\n" for name in selected).encode())
        self.assertEqual(len(self.fixture.requests), len(selected))
        self.fixture.requests.clear()
        for missing in ("a", "p06000x", "z"):
            result = self.run_nvc(args=(selected[0], missing))
            self.assert_failure(result, missing, "unknown project")
            self.assertEqual(result.stdout, b"")
            self.assert_no_network()
        result = self.run_nvc(config(*names, names[0]), (names[-1],))
        self.assert_failure(result, names[0], "duplicate section")
        self.assertEqual(result.stdout, b"")
        self.assert_no_network()

    def test_fragmented_responses(self):
        body = release("fragmented-β", padding="x" * 150000)
        for length in (len(body), LIMIT, LIMIT + 1):
            with self.subTest(length=length):
                padded = body + b" " * (length - len(body))
                chunks = [padded[i:i + 8191] for i in range(0, len(padded), 8191)]
                self.fixture.add("fragmented", chunks)
                result = self.run_nvc(config("fragmented"))
                if length <= LIMIT:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, "fragmented\tfragmented-β\n".encode())
                else:
                    self.assert_failure(result, "fragmented", "exceeds")
                    self.assertEqual(result.stdout, b"")

    def test_invalid_configs_before_any_request(self):
        cases = {
            "duplicate section": config("good", "good"),
            "duplicate source": config("good") + "source=github\n",
            "duplicate repo": config("good") + "repo=other/repo\n",
            "unknown key": config("good") + "token=secret\n",
            "unknown source": config("good") + "[bad]\nsource=gitlab\nrepo=a/b\n",
            "missing source": config("good") + "[bad]\nrepo=a/b\n",
            "missing repo": config("good") + "[bad]\nsource=github\n",
            "empty source": "[bad]\nsource=\nrepo=a/b\n",
            "empty repo": "[bad]\nsource=github\nrepo=\n",
            "outside section": "source=github\n" + config("good"),
            "empty section": "[]\nsource=github\nrepo=a/b\n",
            "malformed section": "[bad\nsource=github\nrepo=a/b\n",
            "missing equals": config("good") + "broken\n",
            "case sensitive key": "[bad]\nSource=github\nrepo=a/b\n",
            "case sensitive source": "[bad]\nsource=GitHub\nrepo=a/b\n",
            "literal quotes": "[bad]\nsource=\"github\"\nrepo=a/b\n",
            "no inline comments": "[bad]\nsource=github # comment\nrepo=a/b\n",
        }
        self.fixture.add("good")
        for label, text in cases.items():
            with self.subTest(label=label):
                result = self.run_nvc(text)
                self.assert_failure(result)
                self.assertEqual(result.stdout, b"")
                self.assert_no_network()

    def test_unselected_invalid_config_is_rejected(self):
        self.fixture.add("good")
        result = self.run_nvc(config("good") + "[bad]\nsource=github\n", ("good",))
        self.assert_failure(result)
        self.assert_no_network()

    def test_config_boundaries_and_repository_values(self):
        self.fixture.add("good")
        for text in (config("good").replace("\n", "\r\n"), config("good").rstrip("\n")):
            result = self.run_nvc(text)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b"good\tv1.2.3\n")
        invalid = [b"", b"# only comments\n", b"[bad\x00]\n", b"[bad\tname]\n",
                   b"[bad]\nsource=github\nrepo=a/b\x00\n", b"#" + b"x" * LIMIT]
        for repo in ("/name", "owner/", "a/b/c", "a/..", "a/.", "a/b?x", "a/b#x", "a/b x", "a/b%20"):
            invalid.append(f"[bad]\nsource=github\nrepo={repo}\n".encode())
        for text in invalid:
            with self.subTest(text=text[:80]):
                self.fixture.requests.clear()
                result = self.run_nvc(text)
                self.assert_failure(result)
                self.assertEqual(result.stdout, b"")
                self.assert_no_network()
        text = config("good")
        text += "#" + "x" * (LIMIT - len(text) - 1)
        result = self.run_nvc(text)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_nvc(config("good"), ("good", "good"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"good\tv1.2.3\n")

    def test_cli_errors(self):
        cases = [("--unknown",), ("-j",), ("-c",), ("missing",), ("GOOD",)]
        cases += [("-j", value) for value in ("0", "-1", "65", "1x", "1.5", "", "9999999999999999999999999999")]
        for args in cases:
            with self.subTest(args=args):
                result = self.run_nvc(config("good"), args)
                self.assert_failure(result)
                self.assertEqual(result.stdout, b"")
                self.assert_no_network()
        self.assert_failure(self.run_nvc(args=(), use_config=False))
        self.assert_no_network()

    def test_missing_config_file(self):
        self.assert_failure(self.run_nvc())
        self.assert_no_network()

    def test_help_and_version(self):
        for option in ("--help", "--version"):
            with self.subTest(option=option):
                result = self.run_nvc(args=(option,), use_config=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.strip())
                self.assertEqual(result.stderr, b"")
                self.assert_no_network()

    def test_invalid_release_responses(self):
        valid = dict(tag_name="v1", draft=False, prerelease=False)
        bodies = [b"", b"{", b"null", b"[]", b'"v1"', b"42", b"true"]
        for field in valid:
            value = valid.copy()
            del value[field]
            bodies.append(json.dumps(value).encode())
        for tag in (None, 12, True, [], {}, ""):
            bodies.append(json.dumps(dict(valid, tag_name=tag)).encode())
        for field in ("draft", "prerelease"):
            for value in (True, None, 0, 1, "false", [], {}):
                bodies.append(json.dumps(dict(valid, **{field: value})).encode())
        for body in bodies:
            with self.subTest(body=body):
                self.fixture.add("bad", body)
                result = self.run_nvc(config("bad"))
                self.assert_failure(result, "bad")
                self.assertEqual(result.stdout, b"")

    def test_unsafe_tags(self):
        tags = ["v1" + chr(value) + "x" for value in (*range(32), 127)]
        bodies = [release(tag) for tag in tags]
        bodies += [b'{"tag_name":"v\xff","draft":false,"prerelease":false}',
                   b'{"tag_name":"v\xc0\xaf","draft":false,"prerelease":false}',
                   b'{"tag_name":"v\xed\xa0\x80","draft":false,"prerelease":false}']
        for body in bodies:
            with self.subTest(body=body):
                self.fixture.add("unsafe", body)
                result = self.run_nvc(config("unsafe"))
                self.assert_failure(result, "unsafe")
                self.assertEqual(result.stdout, b"")

    def test_http_errors(self):
        for status in (401, 403, 404, 429, 500, 503):
            with self.subTest(status=status):
                self.fixture.add("bad", b'{"message":"fixture failure"}', status)
                result = self.run_nvc(config("bad"))
                self.assert_failure(result, "bad")
                if status == 403:
                    self.assertRegex(result.stderr.lower(), b"rate|permission")
                if status == 429:
                    self.assertIn(b"rate", result.stderr.lower())
                self.assertEqual(result.stdout, b"")

    def test_redirects_are_not_followed(self):
        for status in (301, 302, 303, 307, 308):
            with self.subTest(status=status):
                self.fixture.requests.clear()
                self.fixture.routes["/redirect-target"] = (200, release(), {})
                self.fixture.add("redirect", status=status, headers={"Location": self.fixture.url + "/redirect-target"})
                result = self.run_nvc(config("redirect"), env={"GITHUB_TOKEN": "fixture-secret"})
                self.assert_failure(result, "redirect")
                self.assertEqual(result.stdout, b"")
                self.assertEqual([r[0] for r in self.fixture.requests], ["/repos/owner/redirect/releases/latest"])
                self.assertNotIn(b"fixture-secret", result.stderr)

    def test_response_size_boundary(self):
        body = release("boundary")
        self.fixture.add("boundary", body + b" " * (LIMIT - len(body)))
        result = self.run_nvc(config("boundary"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"boundary\tboundary\n")
        for extra in (1, 65536):
            with self.subTest(extra=extra):
                self.fixture.add("boundary", body + b" " * (LIMIT + extra - len(body)))
                result = self.run_nvc(config("boundary"))
                self.assert_failure(result, "boundary", r"size|large|limit|exceed")
                self.assertEqual(result.stdout, b"")

    def test_timeout_and_partial_success(self):
        self.fixture.add("slow", delay=0.8)
        self.fixture.add("good", release("v2"))
        result = self.run_nvc(config("slow", "good"), env={"NVC_TEST_TIMEOUT_MS": "100"})
        self.assert_failure(result, "slow", r"timed?\s*out|timeout")
        self.assertEqual(result.stdout, b"good\tv2\n")
        self.assertNotIn(b"good", result.stderr)

    def test_partial_failure_preserves_success_order(self):
        self.fixture.add("first", release("v-first"), delay=0.15)
        self.fixture.add("bad", b"not json")
        self.fixture.add("last", release("v-last"))
        result = self.run_nvc(config("first", "bad", "last"))
        self.assert_failure(result, "bad")
        self.assertEqual(result.stdout, b"first\tv-first\nlast\tv-last\n")
        self.assertNotIn(b"first", result.stderr)
        self.assertNotIn(b"last", result.stderr)
        self.assertEqual(len(self.fixture.requests), 3)

    def test_concurrency_and_output_order(self):
        for jobs, expected in (("1", 1), ("3", 3), (None, 8), ("64", 12)):
            with self.subTest(jobs=jobs):
                self.fixture.peak = 0
                self.fixture.requests.clear()
                names = [f"p{i}" for i in range(12)]
                arrived = 0
                active = 0
                peak = 0
                def response():
                    nonlocal arrived, active, peak
                    with self.fixture.lock:
                        arrived += 1
                        active += 1
                        peak = max(peak, active)
                        self.fixture.lock.notify_all()
                        self.fixture.lock.wait_for(lambda: arrived >= expected, timeout=1)
                    time.sleep(0.04)
                    with self.fixture.lock:
                        active -= 1
                    return 200, release(), {}
                for name in names:
                    self.fixture.routes[f"/repos/owner/{name}/releases/latest"] = response
                result = self.run_nvc(config(*names), () if jobs is None else ("-j", jobs))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "".join(f"{name}\tv1.2.3\n" for name in names).encode())
                self.assertEqual(peak, expected)
                self.assertEqual(len(self.fixture.requests), len(names))

    def test_token_header_and_no_leak(self):
        token = "fixture-token_123.abc"
        self.fixture.add("good")
        result = self.run_nvc(config("good"), env={"GITHUB_TOKEN": token})
        self.assertEqual(result.returncode, 0, result.stderr)
        headers = {key.lower(): value for key, value in self.fixture.requests[0][1].items()}
        self.assertIn(headers.get("authorization"), ("Bearer " + token, "token " + token))
        self.assertNotIn(token.encode(), result.stdout + result.stderr)
        self.fixture.add("good", status=401)
        result = self.run_nvc(config("good"), env={"GITHUB_TOKEN": token})
        self.assert_failure(result, "good")
        self.assertNotIn(token.encode(), result.stdout + result.stderr)

    def test_invalid_tokens_before_network(self):
        for control in (*range(1, 33), 127):
            token = "secret" + chr(control) + "suffix"
            with self.subTest(control=control):
                result = self.run_nvc(config("good"), env={"GITHUB_TOKEN": token})
                self.assert_failure(result)
                self.assertEqual(result.stdout, b"")
                self.assertNotIn(b"secret", result.stderr)
                self.assertNotIn(b"suffix", result.stderr)
                self.assert_no_network()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: python3 -I tests/test.py ./nvc-test")
    BINARY = str(Path(sys.argv.pop(1)).resolve())
    if not Path(BINARY).is_file():
        raise SystemExit(f"test binary not found: {BINARY}")
    unittest.main()
