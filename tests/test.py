from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler
from socketserver import ThreadingTCPServer


BINARY = None
LIMIT = 1024 * 1024


def release(tag="v1.2.3", **fields):
    return json.dumps(dict(tag_name=tag, draft=False, prerelease=False, **fields)).encode()


def config(*names):
    return "".join(f"[{name}]\nsource = github\nrepo = owner/{name}\n" for name in names)


def test_env():
    env = {key: os.environ[key] for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS")
           if key in os.environ}
    env["LC_ALL"] = "C"
    return env


class Fixture:
    def __init__(self):
        self.routes = {}
        self.requests = []
        self.informational = {}
        self.trailers = {}
        self.lock = threading.Condition()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            disable_nagle_algorithm = True
            timeout = 5

            def handle(self):
                try:
                    super().handle()
                except ConnectionResetError:
                    pass

            def log_message(self, *args):
                pass

            def do_GET(self):
                with fixture.lock:
                    fixture.requests.append((self.path, dict(self.headers)))
                try:
                    route = fixture.routes.get(self.path, (404, b"not found", {}))
                    if callable(route):
                        route = route()
                    status, body, headers = route
                    if self.path in fixture.informational:
                        self.send_response_only(103)
                        for key, value in fixture.informational[self.path].items():
                            self.send_header(key, value)
                        self.end_headers()
                    self.send_response(status)
                    chunks = body if isinstance(body, list) else [body]
                    trailers = fixture.trailers.get(self.path)
                    if trailers is None:
                        self.send_header("Content-Length", str(sum(map(len, chunks))))
                    else:
                        self.send_header("Transfer-Encoding", "chunked")
                        self.send_header("Trailer", ", ".join(trailers))
                    self.send_header("Content-Type", "application/json")
                    for key, value in headers.items():
                        self.send_header(key, value)
                    self.end_headers()
                    for chunk in chunks:
                        if trailers is not None:
                            if not chunk:
                                continue
                            self.wfile.write(f"{len(chunk):x}\r\n".encode())
                        self.wfile.write(chunk)
                        if trailers is not None:
                            self.wfile.write(b"\r\n")
                        self.wfile.flush()
                        if len(chunks) > 1:
                            time.sleep(0.001)
                    if trailers is not None:
                        self.wfile.write(b"0\r\n")
                        for key, value in trailers.items():
                            self.wfile.write(f"{key}: {value}\r\n".encode())
                        self.wfile.write(b"\r\n")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass

        class Server(ThreadingTCPServer):
            request_queue_size = 128

        self.server = Server(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def add(self, name, body=None, status=200, headers=None):
        self.routes[f"/repos/owner/{name}/releases/latest"] = (
            status, release() if body is None else body, headers or {}
        )

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

    def fixture_env(self, overrides=None):
        env = test_env()
        env.update(NVC_TEST_URL=self.fixture.url, NVC_TEST_TIMEOUT_MS="5000")
        env.update(overrides or {})
        return env

    def command(self, args, use_config=True):
        args = list(args)
        command = [BINARY]
        if args and args[0] in ("check", "compare", "accept"):
            command.append(args.pop(0))
        if use_config:
            command += ["-c", str(self.path)]
        return command + args

    def run_nvc(self, text=None, args=(), env=None, use_config=True):
        if text is not None:
            self.path.write_bytes(text if isinstance(text, bytes) else text.encode())
        return subprocess.run(self.command(args, use_config), env=self.fixture_env(env),
                              capture_output=True, timeout=15)

    def assert_result(self, result, stdout):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(result.stdout, stdout)

    def assert_failure(self, result, name=None, pattern=None):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(result.stderr.strip())
        if name is not None:
            self.assertIn(name.encode(), result.stderr)
        if pattern is not None:
            self.assertRegex(result.stderr.decode(errors="replace").lower(), pattern)

    def assert_no_network(self):
        self.assertEqual(self.fixture.requests, [])

    def test_numeric_tags(self):
        route = "/repos/owner/good/tags?per_page=100&page="
        text = "[good]\nsource=github-tags\nrepo=owner/good\nsort=numeric\n"
        cases = [(["16.2", "17.0"], "17.0"), (["9", "10"], "10"),
                 (["1.9.99", "1.10.0"], "1.10.0"),
                 (["18446744073709551615", "18446744073709551616"], "18446744073709551616"),
                 (["9" * 2000, "1" + "0" * 2000], "1" + "0" * 2000),
                 (["01.00", "1", "1.0.0"], "01.00"),
                 (["0", "0.0.1", "0.0"], "0.0.1")]
        for values, expected in cases:
            with self.subTest(values=values):
                self.fixture.routes[route + "1"] = (200, json.dumps([{"name": v} for v in values]).encode(), {})
                result = self.run_nvc(text)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, ("good\t" + expected + "\n").encode())
        self.fixture.routes[route + "1"] = (200, b'[{"name":"16.2"},{"name":"17.0"}]', {})
        self.assertEqual(self.run_nvc(text.replace("sort=numeric\n", "")).stdout, b"good\t16.2\n")
        for bad in ("1.", ".1", "1..2", "v1", "1-rc", "1+2", "-1", "1 2", "١.2"):
            self.fixture.routes[route + "1"] = (200, json.dumps([{"name": "20"}, {"name": bad}]).encode(), {})
            result = self.run_nvc(text)
            self.assert_failure(result, "good", "numeric")
            self.assertEqual(result.stdout, b"")
        self.fixture.routes[route + "1"] = (200, b'[{"name":"nightly"},{"name":"v17.0"},{"name":"v18.0"}]', {})
        result = self.run_nvc(text + "include_regex=^v\nexclude_regex=18\nprefix=v\n")
        self.assertEqual(result.stdout, b"good\t17.0\n")
        for source in ("github", "gitlab", "forgejo", "gitea", "regex", "github-commit"):
            self.assert_failure(self.run_nvc(text.replace("github-tags", source)))
        for option in ("sort=api", "sort=Numeric", "sort=", "sort=numeric\nsort=numeric"):
            self.assert_failure(self.run_nvc(text.replace("sort=numeric", option)))

    def test_numeric_other_providers(self):
        for source, route in (
            ("gitlab-tags", "/api/v4/projects/owner%2Fgood/repository/tags?order_by=updated&sort=desc&per_page=100&page="),
            ("forgejo-tags", "/api/v1/repos/owner/good/tags?limit=100&page="),
            ("gitea-tags", "/api/v1/repos/owner/good/tags?limit=100&page="),
        ):
            text = f"[good]\nsource={source}\nhost=https://example.org\nrepo=owner/good\nsort=numeric\n"
            self.fixture.routes[route + "1"] = (200, json.dumps([{"name": "16.2"}] * 100).encode(), {})
            self.fixture.routes[route + "2"] = (200, b'[{"name":"17.0"}]', {})
            result = self.run_nvc(text)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b"good\t17.0\n")
            for body in (b"[]", b'[{"name":42}]', b"malformed", b'[{}]'):
                self.fixture.routes[route + "1"] = (200, body, {})
                result = self.run_nvc(text)
                self.assert_failure(result, "good")
                self.assertEqual(result.stdout, b"")

    def test_numeric_pagination_and_state(self):
        route = "/repos/owner/good/tags?per_page=100&page="
        text = "[good]\nsource=github-tags\nrepo=owner/good\nsort=numeric\n"
        self.fixture.routes[route + "1"] = (200, b'[{"name":"17.0"}]', {"Link": '<https://example.org>; rel="next"'})
        self.fixture.routes[route + "2"] = (200, b'[{"name":"16.2"}]', {})
        state = str(Path(self.directory.name) / "state.json")
        result = self.run_nvc(text, ("check", "-s", state))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"good\t17.0\n")
        saved = json.loads(Path(state).read_text())
        self.assertEqual(saved["entries"]["good"]["identity"][9:], [None, "numeric"])
        result = self.run_nvc(text.replace("sort=numeric\n", ""), ("compare", "-s", state))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"unchecked", result.stdout)
        self.assert_failure(self.run_nvc(None, ("accept", "-s", state)))
        self.assertEqual(json.loads(Path(state).read_text()), saved)
        self.assertEqual(self.run_nvc(text, ("accept", "-s", state)).returncode, 0)
        original = Path(state).read_bytes()
        for tail in ([None, "api"], ["branch", "numeric"], [None, None], [None, 1]):
            malformed = json.loads(original)
            malformed["entries"]["good"]["identity"][9:] = tail
            data = json.dumps(malformed).encode()
            Path(state).write_bytes(data)
            self.assert_failure(self.run_nvc(text, ("compare", "-s", state)))
            self.assertEqual(Path(state).read_bytes(), data)
        Path(state).write_bytes(original)
        self.fixture.routes[route + "2"] = (200, b'[{"name":"18.0"}]', {})
        self.assertEqual(self.run_nvc(text).stdout, b"good\t18.0\n")
        self.fixture.routes[route + "2"] = (500, b"failed", {})
        result = self.run_nvc(text, ("check", "-s", state))
        self.assert_failure(result, "good", "http 500")
        self.assertEqual(result.stdout, b"")
        self.assert_failure(self.run_nvc(None, ("accept", "-s", state)))
        for page in range(1, 101):
            self.fixture.routes[route + str(page)] = (200, b'[{"name":"17.0"}]', {"Link": '<https://example.org>; rel="next"'})
        result = self.run_nvc(text)
        self.assert_failure(result, "good", "100 pages")
        self.assertEqual(result.stdout, b"")
        self.fixture.routes[route + "100"] = (200, b"[]", {})
        self.assertEqual(self.run_nvc(text).stdout, b"good\t17.0\n")

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
        cases = [("--unknown",), ("-j",), ("-c",), ("-c", ""),
                 ("-s", "state.json"), ("check",), ("compare",), ("accept",),
                 ("check", "-s", ""), ("missing",), ("GOOD",)]
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

    def test_stdout_failures(self):
        self.fixture.add("good")
        self.assert_result(self.run_nvc(config("good"), self.state_args("check")),
                           b"good\tv1.2.3\n")
        for args in (("--help",), ("--version",), (), self.state_args("compare")):
            with self.subTest(args=args), open(os.devnull, "rb") as output:
                result = subprocess.run(self.command(args), env=self.fixture_env(),
                                        stdout=output, stderr=subprocess.PIPE, timeout=15)
                self.assert_failure(result, pattern="cannot write stdout")

    def test_json_document_validation(self):
        body = release("v1")
        self.fixture.add("good", body)
        self.assert_result(self.run_nvc(config("good")), b"good\tv1\n")
        for value in (b'"abc\\\\uD800"', b'"\\uD83D\\uDE00"', b'-1.2e+3',
                      b'{"key":null,"nested":{"key":null}}', b'[{},[],true,false,null,0]'):
            with self.subTest(valid=value):
                self.fixture.add("good", body[:-1] + b',"extra":' + value + b"}")
                self.assert_result(self.run_nvc(config("good")), b"good\tv1\n")
        malformed = [body + b"\x00", body + b"{}", body[:-1] + b',"tag_name":"v2"}',
                     body[:-1] + b',"tag_\\u006eame":"v2"}', body[:-1] + b',"x\\u0000":null}',
                     body[:-1] + b',"extra":' + b"[" * 64 + b"0" + b"]" * 64 + b"}"]
        malformed.extend(body[:-1] + b',"extra":' + value + b"}" for value in
                         (b"NaN", b"Infinity", b"-01", b"1.", b"1.e2", b'"\\uD800"',
                          b'"\\uDC00"', b'"\\uD800\\u0061"', b'"literal\nnewline"',
                          b'{"key":null,"key":null}', b"[1,]", b"{\"x\":0,}",
                          b"/*comment*/1", b"01", b"+1", b"1e+", b"1e"))
        for data in malformed:
            with self.subTest(data=data):
                self.fixture.add("good", data)
                result = self.run_nvc(config("good"))
                self.assert_failure(result, "good", "invalid source response")
                self.assertEqual(result.stdout, b"")

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

    def test_response_header_size_boundary(self):
        prefix = "X-Boundary: "
        for size in (8192, 8193):
            with self.subTest(size=size):
                self.fixture.add("good", headers={"X-Boundary": "x" * (size - len(prefix) - 2)})
                result = self.run_nvc(config("good"))
                if size == 8192:
                    self.assert_result(result, b"good\tv1.2.3\n")
                else:
                    self.assert_failure(result, "good", "response header exceeds 8 kib")
                    self.assertEqual(result.stdout, b"")

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
        release_slow = threading.Event()
        self.fixture.routes["/repos/owner/slow/releases/latest"] = (
            lambda: (200, release(), {}) if release_slow.wait(5) else (500, b"", {})
        )
        self.fixture.add("good", release("v2"))
        try:
            result = self.run_nvc(config("slow", "good"), env={"NVC_TEST_TIMEOUT_MS": "1000"})
        finally:
            release_slow.set()
        self.assert_failure(result, "slow", r"timed?\s*out|timeout")
        self.assertEqual(result.stdout, b"good\tv2\n")
        self.assertNotIn(b"good", result.stderr)

    def test_partial_failure_preserves_success_order(self):
        later_started = threading.Event()
        self.fixture.routes["/repos/owner/first/releases/latest"] = (
            lambda: (200, release("v-first"), {}) if later_started.wait(4) else (500, b"", {})
        )
        self.fixture.add("last", release("v-last"))

        def bad_response():
            later_started.set()
            return 200, b"not json", {}

        self.fixture.routes["/repos/owner/bad/releases/latest"] = bad_response
        result = self.run_nvc(config("first", "last", "bad"), ("-j", "2"))
        self.assert_failure(result, "bad")
        self.assertEqual(result.stdout, b"first\tv-first\nlast\tv-last\n")
        self.assertNotIn(b"first", result.stderr)
        self.assertNotIn(b"last", result.stderr)
        self.assertEqual(len(self.fixture.requests), 3)

    def test_concurrency_and_output_order(self):
        for jobs, expected in (("1", 1), ("3", 3), (None, 8), ("64", 12)):
            with self.subTest(jobs=jobs):
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
                        ready = self.fixture.lock.wait_for(lambda: arrived >= expected, timeout=4)
                    time.sleep(0.04)
                    with self.fixture.lock:
                        active -= 1
                    return (200, release(), {}) if ready else (500, b"", {})
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

    def test_invalid_token_isolated(self):
        self.fixture.add("good")
        text = config("bad") + "token_env=NVC_BAD_TOKEN\n" + config("good")
        result = self.run_nvc(text, env={"NVC_BAD_TOKEN": "invalid token"})
        self.assert_failure(result, "bad", "invalid authentication token")
        self.assertEqual(result.stdout, b"good\tv1.2.3\n")
        self.assertEqual(len(self.fixture.requests), 1)

    def state_args(self, command, *names):
        return (command, "-s", str(Path(self.directory.name) / "state.json"), *names)

    def test_sources_filters_and_prefix(self):
        cases = [
            ("github-tags", "", "/repos/owner/good/tags?per_page=100&page=1", [{"name": "skip"}, {"name": "vv2"}]),
            ("gitlab", "host=https://gitlab.example:8443\n", "/api/v4/projects/owner%2Fgood/releases?order_by=released_at&sort=desc&per_page=100&page=1", [{"tag_name": "vv0", "upcoming_release": True}, {"tag_name": "vv2"}]),
            ("forgejo", "host=https://forge.example:8443\n", "/api/v1/repos/owner/good/releases?draft=false&pre-release=false&limit=100&page=1", [dict(tag_name="vv0", draft=True, prerelease=False), dict(tag_name="vv2", draft=False, prerelease=False)]),
            ("gitea", "host=https://forge.example\n", "/api/v1/repos/owner/good/releases?draft=false&pre-release=false&limit=100&page=1", [dict(tag_name="vv2", draft=False, prerelease=False)]),
        ]
        for source, host, route, body in cases:
            with self.subTest(source=source):
                self.fixture.requests.clear()
                self.fixture.routes[route] = (200, json.dumps(body).encode(), {})
                text = f"[good]\nsource={source}\nrepo=owner/good\n{host}include_regex=^vv\nexclude_regex=0$\nprefix=v\n"
                result = self.run_nvc(text, env={"GITHUB_TOKEN": "github-only"})
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, b"good\tv2\n")
                if source != "github-tags":
                    self.assertNotIn("Authorization", self.fixture.requests[0][1])

    def test_explicit_tags_pagination_and_errors(self):
        cases = [
            ("gitlab-tags", "/api/v4/projects/owner%2Fgood/repository/tags?order_by=updated&sort=desc&per_page=100&page="),
            ("forgejo-tags", "/api/v1/repos/owner/good/tags?limit=100&page="),
            ("gitea-tags", "/api/v1/repos/owner/good/tags?limit=100&page="),
        ]
        for source, route in cases:
            text = f"[good]\nsource={source}\nrepo=owner/good\nhost=https://example.org\ninclude_regex=^vv\nprefix=v\n"
            with self.subTest(source=source):
                self.fixture.routes[route + "1"] = (200, b'[{"name":"skip"}]', {"Link": '<https://evil.example/>; rel="next"'})
                self.fixture.routes[route + "2"] = (200, b'[{"name":"vv2"},{"name":"vv99"}]', {})
                self.fixture.requests.clear()
                result = self.run_nvc(text)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, b"good\tv2\n")
                self.assertEqual([r[0] for r in self.fixture.requests], [route + "1", route + "2"])
                for body in (b'{}', b'[]', b'[{}]', b'[{"name":null}]', b'[{"name":""}]', b'[{"name":"x\\n"}]', json.dumps([{"name": "skip"}] * 101).encode()):
                    self.fixture.routes[route + "1"] = (200, body, {})
                    self.assert_failure(self.run_nvc(text), "good")

    def test_branch_commits_and_authentication(self):
        cases = [
            ("github", "", "/repos/owner/good/branches/feature%2Fx%3Fq%23z", "sha", "Authorization", "Bearer explicit"),
            ("gitlab", "host=https://example.org\n", "/api/v4/projects/owner%2Fgood/repository/branches/feature%2Fx%3Fq%23z", "id", "PRIVATE-TOKEN", "explicit"),
            ("forgejo", "host=https://example.org\n", "/api/v1/repos/owner/good/branches/feature%2Fx%3Fq%23z", "id", "Authorization", "Bearer explicit"),
            ("gitea", "host=https://example.org\n", "/api/v1/repos/owner/good/branches/feature%2Fx%3Fq%23z", "id", "Authorization", "Bearer explicit"),
        ]
        for provider, host, route, field, header, value in cases:
            text = f"[good]\nsource={provider}-commit\nrepo=owner/good\n{host}branch=feature/x?q#z\ntoken_env=NVC_FIXTURE_TOKEN\n"
            with self.subTest(provider=provider):
                for length in (40, 64):
                    commit = "aB12" * (length // 4)
                    self.fixture.routes[route] = (200, json.dumps({"commit": {field: commit}}).encode(), {"Link": '<https://evil.example/>; rel="next"'})
                    self.fixture.requests.clear()
                    result = self.run_nvc(text, env={"NVC_FIXTURE_TOKEN": "explicit", "GITHUB_TOKEN": "github-only"})
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, f"good\t{commit}\n".encode())
                    self.assertEqual(len(self.fixture.requests), 1)
                    self.assertEqual(self.fixture.requests[0][1][header], value)
                    self.assertNotIn(b"explicit", result.stdout + result.stderr)
                    self.assert_failure(self.run_nvc(text + "exclude_regex=^a\n"), "good", "no matching")
                for body in (b'[]', b'{}', b'{"commit":null}', b'{"commit":[]}', b'{"commit":{}}', json.dumps({"commit": {field: 12}}).encode(), *[json.dumps({"commit": {field: v}}).encode() for v in ("", "a" * 39, "a" * 41, "a" * 63, "a" * 65, "g" * 40, "a" * 39 + "\n", "a" * 39 + "\0")]):
                    self.fixture.routes[route] = (200, body, {})
                    self.assert_failure(self.run_nvc(text), "good")
                self.fixture.routes[route] = (404, b'{}', {})
                self.fixture.requests.clear()
                self.assert_failure(self.run_nvc(text), "good", "404")
                self.assertEqual(len(self.fixture.requests), 1)
                for bad in (text.replace("branch=feature/x?q#z\n", ""), text + "branch=other\n", text + "prefix=a\n", text.replace("branch=feature/x?q#z", "branch="), text.replace("-commit", "-tags"), text.replace("feature/x?q#z", "."), text.replace("feature/x?q#z", "..")):
                    self.fixture.requests.clear()
                    self.assert_failure(self.run_nvc(bad))
                    self.assert_no_network()

    def test_branch_identity_and_legacy_state(self):
        self.fixture.add("legacy")
        route = "/repos/owner/good/branches/"
        for branch in ("main", "next"):
            self.fixture.routes[route + branch] = (200, json.dumps({"commit": {"sha": "a" * 40}}).encode(), {})
        text = config("legacy") + "[good]\nsource=github-commit\nrepo=owner/good\nbranch=main\n"
        result = self.run_nvc(text, self.state_args("check"))
        self.assertEqual(result.returncode, 0, result.stderr)
        state = Path(self.directory.name) / "state.json"
        saved = json.loads(state.read_text())
        self.assertEqual(len(saved["entries"]["legacy"]["identity"]), 9)
        self.assertEqual(saved["entries"]["good"]["identity"][9], "main")
        self.assertEqual(self.run_nvc(args=self.state_args("accept")).returncode, 0)
        before = state.read_bytes()
        changed = text.replace("branch=main", "branch=next")
        self.fixture.requests.clear()
        self.assert_failure(self.run_nvc(changed, self.state_args("accept")))
        self.assertEqual(state.read_bytes(), before)
        result = self.run_nvc(args=self.state_args("compare", "good"))
        self.assertEqual(result.stdout, b"good\tunchecked\t\t\n")
        self.assert_no_network()
        self.assertEqual(self.run_nvc(args=self.state_args("check", "good")).returncode, 0)
        self.assertEqual(json.loads(state.read_text())["entries"]["legacy"], json.loads(before)["entries"]["legacy"])
        for tail in ([None], ["main", "extra"], [12]):
            invalid = json.loads(before)
            invalid["entries"]["good"]["identity"] = invalid["entries"]["good"]["identity"][:9] + tail
            state.write_text(json.dumps(invalid))
            previous = state.read_bytes()
            self.assert_failure(self.run_nvc(args=self.state_args("accept")))
            self.assertEqual(state.read_bytes(), previous)

    def test_pagination_and_regex(self):
        self.fixture.routes["/repos/owner/good/tags?per_page=100&page=1"] = (200, json.dumps([{"name": "skip"}] * 100).encode(), {})
        self.fixture.routes["/repos/owner/good/tags?per_page=100&page=2"] = (200, b'[{"name":"v2"}]', {})
        result = self.run_nvc("[good]\nsource=github-tags\nrepo=owner/good\ninclude_regex=^v\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"good\tv2\n")
        self.fixture.routes["/versions"] = (200, b"release=v2 release=v3", {})
        text = "[good]\nsource=regex\nurl=https://example.org/versions\npattern=release=(v[0-9]+)\nprefix=v\n"
        result = self.run_nvc(text)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"good\t2\n")
        for body in (b"release=v2\x00hidden", b"nothing"):
            self.fixture.routes["/versions"] = (200, body, {})
            self.assert_failure(self.run_nvc(text))

    def test_source_configuration_security(self):
        for host in ("http://example.org", "https://user:secret@example.org", "https://example.org/#x", "https://example.org#", "https://example.org?x", "https://example.org/path", "https:///example.org", "https://"):
            self.assert_failure(self.run_nvc(f"[good]\nsource=forgejo\nrepo=owner/good\nhost={host}\n"))
            self.assert_no_network()
        for expression in ("[", "no-capture"):
            self.assert_failure(self.run_nvc(f"[good]\nsource=regex\nurl=https://example.org/versions\npattern={expression}\n"))
            self.assert_no_network()
        route = "/api/v1/repos/owner/good/releases?draft=false&pre-release=false&limit=100&page=1"
        self.fixture.routes[route] = (200, b'[{"tag_name":"v1","draft":false,"prerelease":false}]', {})
        result = self.run_nvc("[good]\nsource=forgejo\nrepo=owner/good\nhost=https://example.org:8443\ntoken_env=FORGE_TOKEN\n", env={"FORGE_TOKEN": "forge-secret", "GITHUB_TOKEN": "github-secret"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fixture.requests[-1][1]["Authorization"], "Bearer forge-secret")
        self.assertNotIn(b"secret", result.stdout + result.stderr)

    def test_regex_urls_and_first_capture(self):
        text = "[good]\nsource=regex\nurl=https://example.org?format=versions\npattern=release=(v[0-9]+)\n"
        route = "/?format=versions"
        self.fixture.routes[route] = (200, b"release=v1 release=v2", {})
        self.assert_result(self.run_nvc(text), b"good\tv1\n")
        self.assertEqual([r[0] for r in self.fixture.requests], [route])
        for suffix in ("include_regex=^v2$\n", "exclude_regex=^v1$\n"):
            result = self.run_nvc(text + suffix)
            self.assert_failure(result, "good", "no matching candidate")
            self.assertEqual(result.stdout, b"")
        self.fixture.routes[route] = (200, b"release=", {})
        result = self.run_nvc(text.replace("(v[0-9]+)", "(v[0-9]+)?"))
        self.assert_failure(result, "good", "invalid source response")
        self.assertEqual(result.stdout, b"")
        for url in ("https://example.org#", "https://example.org/#fragment",
                    "https:///example.org", "https://user:secret@example.org/versions"):
            with self.subTest(url=url):
                self.fixture.requests.clear()
                result = self.run_nvc(text.replace("https://example.org?format=versions", url))
                self.assert_failure(result, "good", "invalid source configuration")
                self.assert_no_network()
                self.assertNotIn(b"secret", result.stderr)

    def test_nested_gitlab_repository(self):
        route = "/api/v4/projects/group%2Fsubgroup%2Fgood/releases?order_by=released_at&sort=desc&per_page=100&page=1"
        self.fixture.routes[route] = (200, b'[{"tag_name":"v2"}]', {})
        text = "[good]\nsource=gitlab\nhost=https://example.org\nrepo=group/subgroup/good\n"
        self.assert_result(self.run_nvc(text), b"good\tv2\n")
        self.assertEqual([r[0] for r in self.fixture.requests], [route])

    def test_total_count_pagination(self):
        cases = (
            ("gitea-tags", "/api/v1/repos/owner/good/tags?limit=100&page=", "X-Total-Count"),
            ("forgejo-tags", "/api/v1/repos/owner/good/tags?limit=100&page=", "X-Total-Count"),
            ("gitlab-tags", "/api/v4/projects/owner%2Fgood/repository/tags?order_by=updated&sort=desc&per_page=100&page=", "X-Total"),
        )
        first = json.dumps([{"name": "16.2"}] * 50).encode()
        for source, route, header in cases:
            with self.subTest(source=source):
                self.fixture.requests.clear()
                self.fixture.routes[route + "1"] = (200, first, {header: "51"})
                self.fixture.routes[route + "2"] = (200, b'[{"name":"17.0"}]', {})
                text = f"[good]\nsource={source}\nrepo=owner/good\nhost=https://example.org\nsort=numeric\n"
                self.assert_result(self.run_nvc(text), b"good\t17.0\n")
                self.assertEqual([r[0] for r in self.fixture.requests], [route + "1", route + "2"])
                self.fixture.requests.clear()
                self.fixture.routes[route + "1"] = (200, first, {header: "51"})
                self.assert_result(self.run_nvc(text.replace("sort=numeric\n", "include_regex=^17\n")),
                                   b"good\t17.0\n")
                self.assertEqual([r[0] for r in self.fixture.requests], [route + "1", route + "2"])

    def test_pagination_completion(self):
        route = "/api/v1/repos/owner/good/tags?limit=100&page="
        text = "[good]\nsource=gitea-tags\nrepo=owner/good\nhost=https://example.org\nsort=numeric\n"
        paths = [route + str(page) for page in range(1, 4)]
        self.fixture.routes[paths[0]] = (200, b'[{"name":"1"}]', {"X-Total-Count": "3"})
        self.fixture.routes[paths[1]] = (200, b'[{"name":"2"}]', {})
        self.fixture.routes[paths[2]] = (200, b'[{"name":"3"}]', {})
        self.assert_result(self.run_nvc(text), b"good\t3\n")
        self.assertEqual([r[0] for r in self.fixture.requests], paths)
        self.fixture.requests.clear()
        self.fixture.routes[paths[2]] = (200, b"[]", {})
        result = self.run_nvc(text)
        self.assert_failure(result, "good", "empty page")
        self.assertEqual(result.stdout, b"")
        self.assertEqual([r[0] for r in self.fixture.requests], paths)

    def test_pagination_replaces_totals(self):
        route = "/api/v1/repos/owner/good/tags?limit=100&page="
        text = "[good]\nsource=gitea-tags\nrepo=owner/good\nhost=https://example.org\nsort=numeric\n"
        for totals, expected in (((100, 2), "2"), ((2, 3), "3")):
            with self.subTest(totals=totals):
                self.fixture.requests.clear()
                for page, total in enumerate(totals, 1):
                    self.fixture.routes[route + str(page)] = (
                        200, json.dumps([{"name": str(page)}]).encode(), {"X-Total-Count": str(total)},
                    )
                self.fixture.routes[route + "3"] = (200, b'[{"name":"3"}]', {})
                self.assert_result(self.run_nvc(text), f"good\t{expected}\n".encode())
                self.assertEqual([r[0] for r in self.fixture.requests],
                                 [route + str(page) for page in range(1, int(expected) + 1)])

    def test_pagination_at_page_cap(self):
        route = "/api/v1/repos/owner/good/tags?limit=100&page="
        text = "[good]\nsource=gitea-tags\nrepo=owner/good\nhost=https://example.org\nsort=numeric\n"
        paths = [route + str(page) for page in range(1, 101)]
        full = json.dumps([{"name": "1"}] * 100).encode()
        for path in paths[:-1]:
            self.fixture.routes[path] = (200, full, {})
        short = b'[{"name":"2"}]'
        for body, headers, complete in (
            (short, {"X-Total-Count": "9901"}, True),
            (full, {"X-Total-Count": "10000"}, False),
            (short, {"X-Total-Count": "9901", "X-Next-Page": "101"}, False),
        ):
            with self.subTest(headers=headers):
                self.fixture.requests.clear()
                self.fixture.routes[paths[-1]] = (200, body, headers)
                result = self.run_nvc(text)
                if complete:
                    self.assert_result(result, b"good\t2\n")
                else:
                    self.assert_failure(result, "good", "100 pages")
                    self.assertEqual(result.stdout, b"")
                self.assertEqual([r[0] for r in self.fixture.requests], paths)

    def test_link_relation_parameters(self):
        route = "/repos/owner/good/tags?per_page=100&page="
        text = "[good]\nsource=github-tags\nrepo=owner/good\nsort=numeric\n"
        self.fixture.routes[route + "2"] = (200, b'[{"name":"2"}]', {})
        for link, next_page in (
            ('<https://example.org/?rel=next>; rel=prev', False),
            ('</>; rel=next-page', False),
            ('</>; rel=next; anchor="/other"', False),
            ('</>; rel=prev; rel=next', False),
            ('</>; rel="next', False),
            ('</previous>; rel=prev, </more>; title="a, b"; rel="alternate NEXT"', True),
        ):
            with self.subTest(link=link):
                self.fixture.requests.clear()
                self.fixture.routes[route + "1"] = (200, b'[{"name":"1"}]', {"Link": link})
                self.assert_result(self.run_nvc(text), b"good\t2\n" if next_page else b"good\t1\n")
                paths = [route + "1", route + "2"] if next_page else [route + "1"]
                self.assertEqual([r[0] for r in self.fixture.requests], paths)

    def test_informational_pagination_headers_are_ignored(self):
        route = "/repos/owner/good/tags?per_page=100&page=1"
        self.fixture.informational[route] = {
            "Link": '</more>; rel="next"', "X-Next-Page": "2", "X-Total-Count": "2",
        }
        self.fixture.routes[route] = (200, b'[{"name":"1"}]', {})
        text = "[good]\nsource=github-tags\nrepo=owner/good\nsort=numeric\n"
        self.assert_result(self.run_nvc(text), b"good\t1\n")
        self.assertEqual([r[0] for r in self.fixture.requests], [route])

    def test_trailer_pagination_headers_are_ignored(self):
        route = "/repos/owner/good/tags?per_page=100&page="
        self.fixture.routes[route + "1"] = (200, b'[{"name":"1"}]', {})
        self.fixture.trailers[route + "1"] = {
            "Link": '</more>; rel="next"', "X-Next-Page": "2", "X-Total-Count": "2",
        }
        text = "[good]\nsource=github-tags\nrepo=owner/good\nsort=numeric\n"
        self.assert_result(self.run_nvc(text), b"good\t1\n")
        self.assertEqual([r[0] for r in self.fixture.requests], [route + "1"])
        self.fixture.requests.clear()
        self.fixture.routes[route + "1"] = (200, b'[{"name":"1"}]', {"X-Total-Count": "2"})
        self.fixture.trailers[route + "1"] = {"X-Total-Count": "1"}
        self.fixture.routes[route + "2"] = (200, b'[{"name":"2"}]', {})
        self.assert_result(self.run_nvc(text), b"good\t2\n")
        self.assertEqual([r[0] for r in self.fixture.requests], [route + "1", route + "2"])

    def test_dot_repository_components(self):
        for source, host, repo in (
            ("github", "", "./good"),
            ("forgejo", "host=https://example.org\n", "../good"),
            ("gitlab", "host=https://example.org\n", "owner/./good"),
        ):
            self.assert_failure(self.run_nvc(f"[good]\nsource={source}\n{host}repo={repo}\n"))
        self.assert_no_network()

    def test_invalid_utf8_configuration(self):
        for text in (b"[g\xffood]\nsource=github\nrepo=owner/good\n",
                     config("good").encode() + b"prefix=\xc3\n"):
            self.assert_failure(self.run_nvc(text, self.state_args("check")), pattern="utf-8")
        self.assertEqual(self.fixture.requests, [])

    def test_short_page_markers_and_page_bound(self):
        cases = [
            ("github-tags", "", "/repos/owner/good/tags?per_page=100&page=", {"name": "skip"}, {"name": "v2"}, {"Link": '<https://evil.example/page>; rel="next"'}),
            ("gitlab", "host=https://example.org\n", "/api/v4/projects/owner%2Fgood/releases?order_by=released_at&sort=desc&per_page=100&page=", {"tag_name": "skip"}, {"tag_name": "v2"}, {"X-Next-Page": "2"}),
            ("forgejo", "host=https://example.org\n", "/api/v1/repos/owner/good/releases?draft=false&pre-release=false&limit=100&page=", dict(tag_name="skip", draft=False, prerelease=False), dict(tag_name="v2", draft=False, prerelease=False), {"Link": '<https://evil.example/page>; rel="next"'}),
        ]
        for source, host, route, first, last, headers in cases:
            with self.subTest(source=source):
                self.fixture.requests.clear()
                self.fixture.routes[route + "1"] = (200, json.dumps([first]).encode(), headers)
                self.fixture.routes[route + "2"] = (200, json.dumps([last]).encode(), {})
                result = self.run_nvc(f"[good]\nsource={source}\nrepo=owner/good\n{host}include_regex=^v\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, b"good\tv2\n")
                self.assertEqual([r[0] for r in self.fixture.requests], [route + "1", route + "2"])
        route = "/repos/owner/good/tags?per_page=100&page="
        for page in range(1, 101):
            self.fixture.routes[route + str(page)] = (200, b'[{"name":"skip"}]', {"Link": '<https://example.org/next>; rel="next"'})
        self.fixture.requests.clear()
        self.assert_failure(self.run_nvc("[good]\nsource=github-tags\nrepo=owner/good\ninclude_regex=^v\n"), pattern="100 pages")
        self.assertEqual(len(self.fixture.requests), 100)

    def test_state_accept_atomic_selection_and_no_credentials(self):
        self.fixture.add("first")
        self.fixture.add("second", status=500)
        text = config("first", "second")
        self.assert_failure(self.run_nvc(text, self.state_args("check"), env={"GITHUB_TOKEN": "never-persist-token"}))
        state = Path(self.directory.name) / "state.json"
        previous = state.read_bytes()
        self.assertNotIn(b"never-persist-token", previous)
        self.assert_failure(self.run_nvc(args=self.state_args("accept")))
        self.assertEqual(state.read_bytes(), previous)
        self.assertEqual(self.run_nvc(args=self.state_args("accept", "first")).returncode, 0)
        self.assertEqual(json.loads(state.read_text())["entries"]["first"]["accepted"], "v1.2.3")

    def test_state_workflow_failures_and_identity(self):
        self.fixture.add("good", release("v1"))
        text = config("good")
        result = self.run_nvc(text, self.state_args("check"))
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_nvc(args=self.state_args("compare"))
        self.assertEqual(result.stdout, b"good\tnew\t\tv1\n")
        self.assertEqual(self.run_nvc(args=self.state_args("accept")).returncode, 0)
        self.assertEqual(self.run_nvc(args=self.state_args("compare")).stdout, b"good\tunchanged\tv1\tv1\n")
        self.fixture.add("good", release("v2"))
        self.assertEqual(self.run_nvc(args=self.state_args("check")).returncode, 0)
        self.assertEqual(self.run_nvc(args=self.state_args("compare")).stdout, b"good\tchanged\tv1\tv2\n")
        self.fixture.add("good", status=500)
        self.assert_failure(self.run_nvc(args=self.state_args("check")))
        result = self.run_nvc(args=self.state_args("compare"))
        self.assert_failure(result)
        self.assertEqual(result.stdout, b"good\tfailed\tv1\tv2\n")
        self.assert_failure(self.run_nvc(args=self.state_args("accept")))
        self.assert_failure(self.run_nvc(text + "prefix=v\n", self.state_args("accept")))
        self.assertEqual(self.run_nvc(args=self.state_args("compare")).stdout, b"good\tunchecked\t\t\n")
        state = json.loads((Path(self.directory.name) / "state.json").read_text())
        self.assertEqual(state["entries"]["good"]["pending"], "v2")
        self.assertFalse(state["entries"]["good"]["eligible"])

    def test_state_malformed_and_interrupted(self):
        self.fixture.add("good")
        state = Path(self.directory.name) / "state.json"
        for body in ('{', '{}', '{"format":2,"entries":{}}', '{"format":1,"entries":[]}', '{"format":1,"format":1,"entries":{}}', '{"format":1,"entries":{"a\\u0000b":{}}}'):
            state.write_text(body)
            self.assert_failure(self.run_nvc(config("good"), self.state_args("check")))
            self.assertEqual(state.read_text(), body)
        state.unlink()
        self.assertEqual(self.run_nvc(args=self.state_args("check")).returncode, 0)
        previous = state.read_bytes()
        self.fixture.add("good", release("v2"))
        result = self.run_nvc(args=self.state_args("check"), env={"NVC_TEST_STATE_INTERRUPT": "1"})
        self.assertEqual(result.returncode, 99)
        self.assertEqual(state.read_bytes(), previous)
        self.assertEqual(self.run_nvc(args=self.state_args("check")).returncode, 0)

    def test_offline_commands_ignore_tokens_and_missing_state(self):
        self.path.write_text(config("good"))
        result = self.run_nvc(args=self.state_args("compare"), env={"GITHUB_TOKEN": "invalid token"})
        self.assert_failure(result)
        self.assertEqual(result.stdout, b"good\tunchecked\t\t\n")
        self.assert_no_network()
        self.fixture.add("good")
        self.assertEqual(self.run_nvc(args=self.state_args("check")).returncode, 0)
        result = self.run_nvc(args=self.state_args("accept"), env={"GITHUB_TOKEN": "invalid token"})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_offline_commands_reject_concurrency(self):
        self.path.write_text(config("good"))
        for command in ("compare", "accept"):
            result = self.run_nvc(args=self.state_args(command) + ("-j", "2"))
            self.assert_failure(result, pattern="network check")
            self.assertEqual(result.stdout, b"")
        self.assert_no_network()
        self.assertFalse((Path(self.directory.name) / "state.json.lock").exists())

    def test_state_identity_source_forms(self):
        self.path.write_text(config("good"))
        state = Path(self.directory.name) / "state.json"
        base = ["github", "owner/good"] + [None] * 7
        identities = [base + [None], base + ["main"], base + [None, "numeric"],
                      ["github-commit"] + base[1:],
                      ["github-commit"] + base[1:] + [None],
                      ["unknown"] + base[1:], [None] + base[1:]]
        for identity in identities:
            with self.subTest(identity=identity):
                saved = {"format": 1, "entries": {"good": {
                    "identity": identity, "accepted": None, "pending": "v1", "eligible": True,
                }}}
                body = json.dumps(saved).encode()
                state.write_bytes(body)
                result = self.run_nvc(args=self.state_args("accept"))
                self.assert_failure(result, pattern="invalid state format")
                self.assertEqual(result.stdout, b"")
                self.assertEqual(state.read_bytes(), body)
        self.assert_no_network()

    def test_state_and_lock_file_types(self):
        self.path.write_text(config("good"))
        state = Path(self.directory.name) / "state.json"
        lock = Path(str(state) + ".lock")
        target = Path(self.directory.name) / "target"
        target.write_bytes(b"must remain unchanged")
        for path in (state, lock):
            for kind in ("fifo", "symlink", "directory"):
                with self.subTest(path=path.name, kind=kind):
                    if kind == "fifo":
                        os.mkfifo(path)
                    elif kind == "symlink":
                        path.symlink_to(target)
                    else:
                        path.mkdir()
                    try:
                        result = self.run_nvc(args=self.state_args("compare"))
                        self.assert_failure(result, str(path), "regular file|cannot open state")
                        self.assertEqual(result.stdout, b"")
                        self.assertEqual(target.read_bytes(), b"must remain unchanged")
                    finally:
                        if kind == "directory":
                            path.rmdir()
                        else:
                            path.unlink()
                        if path == state and lock.exists():
                            lock.unlink()
        self.assert_no_network()

    def test_state_write_failure_preserves_previous(self):
        self.fixture.add("good", release("v1"))
        self.assert_result(self.run_nvc(config("good"), self.state_args("check")), b"good\tv1\n")
        state = Path(self.directory.name) / "state.json"
        previous = json.loads(state.read_text())
        previous["entries"]["good"]["accepted"] = "x" * (LIMIT // 2)
        previous["entries"]["good"]["pending"] = "y" * (LIMIT // 2 - 1000)
        body = json.dumps(previous).encode()
        self.assertLessEqual(len(body), LIMIT)
        state.write_bytes(body)
        self.fixture.add("good", release("z" * (LIMIT // 2 + 1000)))
        result = self.run_nvc(args=self.state_args("check"))
        self.assert_failure(result, str(state), "cannot serialize state")
        self.assertEqual(result.stdout, b"good\t" + b"z" * (LIMIT // 2 + 1000) + b"\n")
        self.assertEqual(state.read_bytes(), body)
        self.assertEqual(list(state.parent.glob("state.json.tmp.*")), [])

    def test_truncated_state_corpus(self):
        self.path.write_text(config("good"))
        state = Path(self.directory.name) / "state.json"
        document = '{"format":1,"entries":{"good":{"identity":["github",null,null,null,null,null,null,null,null],"accepted":null,"pending":null,"eligible":false}}}'
        for length in range(len(document)):
            body = document[:length]
            state.write_text(body)
            result = self.run_nvc(args=self.state_args("accept"))
            self.assert_failure(result)
            self.assertEqual(state.read_text(), body)

    def test_state_concurrent_disjoint_merge_and_removed(self):
        names = ("first", "second")
        arrived = threading.Barrier(len(names), timeout=4)

        def response():
            try:
                arrived.wait()
            except threading.BrokenBarrierError:
                return 500, b"", {}
            return 200, release(), {}

        for name in names:
            self.fixture.routes[f"/repos/owner/{name}/releases/latest"] = response
        self.path.write_text(config(*names))
        state = Path(self.directory.name) / "state.json"
        with ThreadPoolExecutor(max_workers=len(names)) as pool:
            jobs = [pool.submit(self.run_nvc, args=self.state_args("check", name))
                    for name in names]
            for name, job in zip(names, jobs):
                self.assert_result(job.result(), f"{name}\tv1.2.3\n".encode())
        self.assertEqual(set(json.loads(state.read_text())["entries"]), set(names))
        self.assert_result(self.run_nvc(config("first"), self.state_args("accept")), b"")
        self.assertIn("second", json.loads(state.read_text())["entries"])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: python3 -I tests/test.py ./nvc-test")
    BINARY = str(Path(sys.argv.pop(1)).resolve())
    try:
        version = subprocess.run([BINARY, "--version"], env=test_env(),
                                 capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"cannot identify test binary: {error}")
    if version.returncode or not version.stdout.startswith(b"nvc-test "):
        raise SystemExit(f"fixture binary required: {BINARY}")
    unittest.main()
