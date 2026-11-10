#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <curl/curl.h>
#include <json-c/json.h>

#define VERSION "0.1.0"
#define RESPONSE_MAX (1024 * 1024)
#define CONFIG_MAX (1024 * 1024)

struct project {
	char *name;
	char *source;
	char *repo;
	bool selected;
	CURL *easy;
	char *body;
	size_t len;
	size_t capacity;
	char *version;
	char error[256];
};

struct config {
	struct project *items;
	struct project **by_name;
	size_t len;
	size_t capacity;
};

static int project_compare(const void *a, const void *b)
{
	const struct project *const *left = a, *const *right = b;

	return strcmp((*left)->name, (*right)->name);
}

static bool controls(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127)
			return true;
	return false;
}

static bool utf8_valid(const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n;) {
		unsigned c = s[i++], value, min, extra;
		if (c < 128)
			continue;
		if (c >= 0xc2 && c <= 0xdf) {
			value = c & 0x1f;
			min = 0x80;
			extra = 1;
		} else if (c >= 0xe0 && c <= 0xef) {
			value = c & 0x0f;
			min = 0x800;
			extra = 2;
		} else if (c >= 0xf0 && c <= 0xf4) {
			value = c & 7;
			min = 0x10000;
			extra = 3;
		} else {
			return false;
		}
		if (extra > n - i)
			return false;
		while (extra--) {
			c = s[i++];
			if ((c & 0xc0) != 0x80)
				return false;
			value = (value << 6) | (c & 0x3f);
		}
		if (value < min || value > 0x10ffff ||
		    (value >= 0xd800 && value <= 0xdfff))
			return false;
	}
	return true;
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return s;
}

static bool repo_valid(const char *s)
{
	const char *slash = strchr(s, '/');

	if (!slash || slash == s || !slash[1] || strchr(slash + 1, '/'))
		return false;
	for (const char *p = s; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || strchr("-._/", *p)))
			return false;
	return strcmp(slash + 1, ".") && strcmp(slash + 1, "..");
}

static void config_free(struct config *cfg)
{
	for (size_t i = 0; i < cfg->len; i++) {
		struct project *p = &cfg->items[i];
		free(p->name);
		free(p->source);
		free(p->repo);
		free(p->body);
		free(p->version);
	}
	free(cfg->by_name);
	free(cfg->items);
}

static int config_read(struct config *cfg, const char *path)
{
	FILE *file = fopen(path, "r");
	char *line = NULL;
	size_t number = 0, total = 0;
	size_t n;
	struct project *p = NULL;
	const char *error = NULL;

	if (!file) {
		fprintf(stderr, "nvc: %s: %s\n", path, strerror(errno));
		return -1;
	}
	line = malloc(CONFIG_MAX + 1);
	if (!line) {
		fclose(file);
		fprintf(stderr, "nvc: out of memory\n");
		return -1;
	}
	while (!feof(file) && !ferror(file)) {
		char *s, *eq, **dest;
		int ch;
		n = 0;
		while ((ch = fgetc(file)) != EOF) {
			if (++total > CONFIG_MAX) {
				error = "configuration exceeds 1 MiB";
				break;
			}
			line[n++] = (char)ch;
			if (ch == '\n')
				break;
		}
		if (error || (!n && ch == EOF))
			break;
		line[n] = '\0';
		number++;
		if (ferror(file)) {
			error = "cannot read configuration";
			break;
		}
		if (memchr(line, '\0', n)) {
			error = "NUL byte in configuration";
			break;
		}
		if (n && line[n - 1] == '\n')
			line[--n] = '\0';
		if (n && line[n - 1] == '\r')
			line[--n] = '\0';
		s = trim(line);
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			size_t len = strlen(s);
			if (len < 3 || s[len - 1] != ']') {
				error = "invalid section";
				break;
			}
			s[len - 1] = '\0';
			s = trim(s + 1);
			if (!*s || controls(s, strlen(s)) || strpbrk(s, "[]")) {
				error = "invalid project name";
				break;
			}
			if (cfg->len == cfg->capacity) {
				size_t capacity = cfg->capacity ? cfg->capacity * 2 : 16;
				if (capacity < cfg->capacity ||
				    capacity > SIZE_MAX / sizeof(*cfg->items)) {
					error = "too many projects";
					break;
				}
				struct project *items = realloc(cfg->items,
					capacity * sizeof(*items));
				if (!items) {
					error = "out of memory";
					break;
				}
				cfg->items = items;
				cfg->capacity = capacity;
			}
			p = &cfg->items[cfg->len++];
			memset(p, 0, sizeof(*p));
			p->name = strdup(s);
			if (!p->name) {
				error = "out of memory";
				break;
			}
			continue;
		}
		eq = strchr(s, '=');
		if (!p || !eq) {
			error = "expected a key inside a section";
			break;
		}
		*eq++ = '\0';
		s = trim(s);
		eq = trim(eq);
		if (!strcmp(s, "source"))
			dest = &p->source;
		else if (!strcmp(s, "repo"))
			dest = &p->repo;
		else {
			error = "unknown key";
			break;
		}
		if (*dest || !*eq || controls(eq, strlen(eq))) {
			error = "duplicate key or invalid empty/control value";
			break;
		}
		*dest = strdup(eq);
		if (!*dest) {
			error = "out of memory";
			break;
		}
		if (feof(file))
			break;
	}
	if (ferror(file))
		error = "cannot read configuration";
	free(line);
	if (fclose(file) && !error)
		error = "cannot close configuration";
	if (error) {
		fprintf(stderr, "nvc: %s:%zu: %s\n", path, number, error);
		return -1;
	}
	if (!cfg->len) {
		fprintf(stderr, "nvc: %s: no projects\n", path);
		return -1;
	}
	cfg->by_name = malloc(cfg->len * sizeof(*cfg->by_name));
	if (!cfg->by_name) {
		fprintf(stderr, "nvc: out of memory\n");
		return -1;
	}
	for (size_t i = 0; i < cfg->len; i++)
		cfg->by_name[i] = &cfg->items[i];
	qsort(cfg->by_name, cfg->len, sizeof(*cfg->by_name), project_compare);
	for (size_t i = 1; i < cfg->len; i++) {
		if (!strcmp(cfg->by_name[i - 1]->name, cfg->by_name[i]->name)) {
			fprintf(stderr, "nvc: %s: duplicate section: %s\n",
				path, cfg->by_name[i]->name);
			return -1;
		}
	}
	for (size_t i = 0; i < cfg->len; i++) {
		p = &cfg->items[i];
		if (!p->source || strcmp(p->source, "github") ||
		    !p->repo || !repo_valid(p->repo)) {
			fprintf(stderr, "nvc: %s: expected source=github and repo=owner/name\n",
				p->name);
			return -1;
		}
	}
	return 0;
}

static size_t body_write(char *data, size_t size, size_t count, void *arg)
{
	struct project *p = arg;
	size_t n;
	char *body;

	if (size && count > SIZE_MAX / size)
		return 0;
	n = size * count;
	if (n > RESPONSE_MAX - p->len) {
		snprintf(p->error, sizeof(p->error), "response exceeds 1 MiB");
		return 0;
	}
	if (p->len + n + 1 > p->capacity) {
		size_t capacity = p->capacity ? p->capacity : 4096;
		while (capacity < p->len + n + 1) {
			if (capacity > (RESPONSE_MAX + 1) / 2) {
				capacity = RESPONSE_MAX + 1;
				break;
			}
			capacity *= 2;
		}
		body = realloc(p->body, capacity);
		if (!body) {
			snprintf(p->error, sizeof(p->error), "out of memory");
			return 0;
		}
		p->body = body;
		p->capacity = capacity;
	}
	memcpy(p->body + p->len, data, n);
	p->len += n;
	p->body[p->len] = '\0';
	return n;
}

static void release_parse(struct project *p)
{
	struct json_tokener *tok = json_tokener_new();
	struct json_object *root = NULL, *tag, *draft, *pre;
	const char *error = "invalid release response";
	size_t end;

	if (!tok) {
		error = "out of memory";
		goto done;
	}
	if (!p->body || !p->len ||
	    !utf8_valid((const unsigned char *)p->body, p->len))
		goto done;
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	root = json_tokener_parse_ex(tok, p->body ? p->body : "", (int)p->len);
	if (json_tokener_get_error(tok) != json_tokener_success)
		goto done;
	end = json_tokener_get_parse_end(tok);
	while (end < p->len && strchr(" \t\r\n", p->body[end]) && p->body[end])
		end++;
	if (end != p->len || !json_object_is_type(root, json_type_object) ||
	    !json_object_object_get_ex(root, "tag_name", &tag) ||
	    !json_object_is_type(tag, json_type_string) ||
	    !json_object_object_get_ex(root, "draft", &draft) ||
	    !json_object_is_type(draft, json_type_boolean) ||
	    !json_object_object_get_ex(root, "prerelease", &pre) ||
	    !json_object_is_type(pre, json_type_boolean))
		goto done;
	if (json_object_get_boolean(draft) || json_object_get_boolean(pre)) {
		error = "release is draft or prerelease";
		goto done;
	}
	const char *value = json_object_get_string(tag);
	size_t len = (size_t)json_object_get_string_len(tag);
	if (!len || controls(value, len) ||
	    !utf8_valid((const unsigned char *)value, len)) {
		error = "invalid version identifier";
		goto done;
	}
	p->version = strdup(value);
	error = p->version ? NULL : "out of memory";
done:
	if (error)
		snprintf(p->error, sizeof(p->error), "%s", error);
	json_object_put(root);
	if (tok)
		json_tokener_free(tok);
}

static int request_start(CURLM *multi, struct project *p,
	struct curl_slist *headers, const char *base, long timeout)
{
	char *url;
	size_t len = strlen(base) + strlen(p->repo) + 32;
	CURLcode code = CURLE_OK;
	CURLMcode mc;

	url = malloc(len);
	p->easy = curl_easy_init();
	if (!url || !p->easy) {
		free(url);
		snprintf(p->error, sizeof(p->error), "out of memory");
		goto fail;
	}
	snprintf(url, len, "%s/repos/%s/releases/latest", base, p->repo);
#define SETOPT(option, value) do { \
	code = curl_easy_setopt(p->easy, option, value); \
	if (code != CURLE_OK) goto setup_fail; \
} while (0)
	SETOPT(CURLOPT_URL, url);
	SETOPT(CURLOPT_HTTPHEADER, headers);
	SETOPT(CURLOPT_USERAGENT, "nvc/" VERSION);
	SETOPT(CURLOPT_WRITEFUNCTION, body_write);
	SETOPT(CURLOPT_WRITEDATA, p);
	SETOPT(CURLOPT_PRIVATE, p);
	SETOPT(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
	SETOPT(CURLOPT_TIMEOUT_MS, timeout);
	SETOPT(CURLOPT_NOSIGNAL, 1L);
	SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
	SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
	SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
	SETOPT(CURLOPT_PROTOCOLS_STR, "https");
#ifdef NVC_TEST
	SETOPT(CURLOPT_PROTOCOLS_STR, "http,https");
	SETOPT(CURLOPT_PROXY, "");
#endif
	if (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2)
		SETOPT(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
#undef SETOPT
	free(url);
	mc = curl_multi_add_handle(multi, p->easy);
	if (mc == CURLM_OK)
		return 0;
	snprintf(p->error, sizeof(p->error), "%s", curl_multi_strerror(mc));
	goto fail;
setup_fail:
	free(url);
	snprintf(p->error, sizeof(p->error), "%s", curl_easy_strerror(code));
fail:
	if (p->easy)
		curl_easy_cleanup(p->easy);
	p->easy = NULL;
	return -1;
}

static int fetch(struct config *cfg, unsigned jobs, const char *token)
{
	CURLM *multi = NULL;
	struct curl_slist *headers = NULL, *next;
	char *auth = NULL;
	const char *base = "https://api.github.com";
	long timeout = 30000;
	size_t cursor = 0;
	unsigned active = 0;
	int status = -1;
	CURLMcode mc = CURLM_OK;

#ifdef NVC_TEST
	const char *test_base = getenv("NVC_TEST_URL");
	const char *test_timeout = getenv("NVC_TEST_TIMEOUT_MS");
	if (test_base)
		base = test_base;
	if (test_timeout)
		timeout = strtol(test_timeout, NULL, 10);
#endif
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		fprintf(stderr, "nvc: cannot initialize curl\n");
		return -1;
	}
	multi = curl_multi_init();
	if (!multi)
		goto done;
	headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	if (!headers)
		goto done;
	next = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
	if (!next)
		goto done;
	headers = next;
	if (token && *token) {
		size_t len = strlen(token) + 32;
		auth = malloc(len);
		if (!auth)
			goto done;
		snprintf(auth, len, "Authorization: Bearer %s", token);
		next = curl_slist_append(headers, auth);
		if (!next)
			goto done;
		headers = next;
	}
	while (cursor < cfg->len || active) {
		while (cursor < cfg->len && active < jobs) {
			struct project *p = &cfg->items[cursor++];
			if (p->selected && !request_start(multi, p, headers, base, timeout))
				active++;
		}
		int running, pending;
		mc = curl_multi_perform(multi, &running);
		if (mc != CURLM_OK)
			goto done;
		CURLMsg *msg;
		while ((msg = curl_multi_info_read(multi, &pending))) {
			struct project *p = NULL;
			long http = 0;
			if (msg->msg != CURLMSG_DONE)
				continue;
			curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &p);
			curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http);
			if (msg->data.result != CURLE_OK) {
				if (!p->error[0])
					snprintf(p->error, sizeof(p->error), "request: %s",
						curl_easy_strerror(msg->data.result));
			} else if (http != 200) {
				snprintf(p->error, sizeof(p->error), "HTTP %ld%s", http,
					http == 404 ? ": no release or repository not accessible" :
					http == 403 ? ": rate limit or permission denied" :
					http == 429 ? ": rate limit exceeded" : "");
			} else {
				release_parse(p);
			}
			curl_multi_remove_handle(multi, p->easy);
			curl_easy_cleanup(p->easy);
			p->easy = NULL;
			free(p->body);
			p->body = NULL;
			active--;
		}
		if (active && (active == jobs || cursor == cfg->len)) {
			mc = curl_multi_poll(multi, NULL, 0, 1000, NULL);
			if (mc != CURLM_OK)
				goto done;
		}
	}
	status = 0;
done:
	if (status)
		fprintf(stderr, "nvc: %s\n", mc != CURLM_OK ?
			curl_multi_strerror(mc) : "cannot allocate HTTP resources");
	for (size_t i = 0; i < cfg->len; i++) {
		struct project *p = &cfg->items[i];
		if (p->easy) {
			curl_multi_remove_handle(multi, p->easy);
			curl_easy_cleanup(p->easy);
			p->easy = NULL;
		}
	}
	if (multi)
		curl_multi_cleanup(multi);
	curl_slist_free_all(headers);
	free(auth);
	curl_global_cleanup();
	return status;
}

static void usage(FILE *out)
{
	fputs("Usage: nvc -c FILE [-j N] [PROJECT ...]\n"
	      "A minimal nv checker in C\n\n"
	      "  -c FILE       INI configuration file\n"
	      "  -j N          Concurrent requests, 1-64 (default: 8)\n"
	      "  -h, --help    Show help\n"
	      "  -V, --version Show version\n", out);
}

int main(int argc, char **argv)
{
	struct config cfg = {0};
	const char *path = NULL, *token = getenv("GITHUB_TOKEN");
	unsigned jobs = 8;
	int opt, status = 1;
	static const struct option options[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};

	opterr = 0;
	while ((opt = getopt_long(argc, argv, "c:j:hV", options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			path = optarg;
			break;
		case 'j': {
			char *end;
			long n;
			errno = 0;
			n = strtol(optarg, &end, 10);
			if (!*optarg || strspn(optarg, "0123456789") != strlen(optarg) ||
			    *end || errno || n < 1 || n > 64) {
				fprintf(stderr, "nvc: concurrency must be 1-64\n");
				return 1;
			}
			jobs = (unsigned)n;
			break;
		}
		case 'h':
			usage(stdout);
			return 0;
		case 'V':
			puts("nvc " VERSION);
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}
	if (!path) {
		usage(stderr);
		return 1;
	}
	if (token)
		for (const unsigned char *s = (const unsigned char *)token; *s; s++)
			if (*s <= 32 || *s >= 127) {
				fprintf(stderr, "nvc: invalid GITHUB_TOKEN\n");
				return 1;
			}
	if (config_read(&cfg, path))
		goto done;
	for (size_t i = 0; i < cfg.len; i++)
		cfg.items[i].selected = optind == argc;
	for (int i = optind; i < argc; i++) {
		struct project key = {.name = argv[i]}, *key_ptr = &key;
		struct project **found = bsearch(&key_ptr, cfg.by_name, cfg.len,
			sizeof(*cfg.by_name), project_compare);
		if (!found) {
			fprintf(stderr, "nvc: unknown project: %s\n", argv[i]);
			goto done;
		}
		(*found)->selected = true;
	}
	if (fetch(&cfg, jobs, token))
		goto done;
	status = 0;
	for (size_t i = 0; i < cfg.len; i++) {
		struct project *p = &cfg.items[i];
		if (!p->selected)
			continue;
		if (p->error[0]) {
			fprintf(stderr, "nvc: %s: %s\n", p->name, p->error);
			status = 1;
		} else if (printf("%s\t%s\n", p->name, p->version) < 0) {
			status = 1;
		}
	}
	if (fflush(stdout) || ferror(stdout)) {
		fprintf(stderr, "nvc: cannot write stdout\n");
		status = 1;
	}
done:
	config_free(&cfg);
	return status;
}
