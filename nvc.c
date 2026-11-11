#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <json-c/json.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERSION "0.1.0"
#define RESPONSE_MAX (1024 * 1024)
#define CONFIG_MAX (1024 * 1024)
#define PAGE_SIZE 100U
#define PAGE_MAX 100U

struct project {
	char *name;
	char *source;
	char *repo;
	char *branch, *sort;
	const struct source_type *type;
	char *host, *url, *regex, *include, *exclude, *prefix, *token_env;
	regex_t extract_re, include_re, exclude_re;
	bool extract_ready, include_ready, exclude_ready;
	unsigned page;
	bool more;
	size_t items_seen, items_total;
	struct curl_slist *headers;
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

enum provider { GITHUB, GITLAB, FORGE, WEB };
enum operation { RELEASE, TAGS, COMMIT, EXTRACT };

static const struct source_type {
	const char *name;
	enum provider provider;
	enum operation operation;
} sources[] = {
	{"github", GITHUB, RELEASE},
	{"github-tags", GITHUB, TAGS},
	{"github-commit", GITHUB, COMMIT},
	{"gitlab", GITLAB, RELEASE},
	{"gitlab-tags", GITLAB, TAGS},
	{"gitlab-commit", GITLAB, COMMIT},
	{"forgejo", FORGE, RELEASE},
	{"forgejo-tags", FORGE, TAGS},
	{"forgejo-commit", FORGE, COMMIT},
	{"gitea", FORGE, RELEASE},
	{"gitea-tags", FORGE, TAGS},
	{"gitea-commit", FORGE, COMMIT},
	{"regex", WEB, EXTRACT},
};

static const struct source_type *source_lookup(const char *name)
{
	for (size_t i = 0; name && i < sizeof(sources) / sizeof(*sources); i++)
		if (!strcmp(name, sources[i].name))
			return &sources[i];
	return NULL;
}

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
		if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
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

static bool repo_valid(const char *s, bool nested)
{
	if (!s)
		return false;
	unsigned separators = 0;
	const char *start = s;
	for (const char *p = s; ; p++) {
		if (*p == '/' || !*p) {
			size_t len = (size_t)(p - start);
			if (!len || (len == 1 && *start == '.') ||
			    (len == 2 && !memcmp(start, "..", 2)))
				return false;
			if (!*p)
				return separators && (nested || separators == 1);
			separators++;
			start = p + 1;
		} else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			     (*p >= '0' && *p <= '9') || strchr("-._", *p)))
			return false;
	}
}

static void config_free(struct config *cfg)
{
	for (size_t i = 0; i < cfg->len; i++) {
		struct project *p = &cfg->items[i];
		free(p->name);
		free(p->source);
		free(p->repo);
		free(p->branch);
		free(p->host);
		free(p->url);
		free(p->regex);
		free(p->include);
		free(p->exclude);
		free(p->sort);
		free(p->prefix);
		free(p->token_env);
		if (p->extract_ready)
			regfree(&p->extract_re);
		if (p->include_ready)
			regfree(&p->include_re);
		if (p->exclude_ready)
			regfree(&p->exclude_re);
		curl_slist_free_all(p->headers);
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
				number++;
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
		if (!utf8_valid((const unsigned char *)line, n)) {
			error = "configuration is not UTF-8";
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
				struct project *items =
				    realloc(cfg->items, capacity * sizeof(*items));
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
		else if (!strcmp(s, "sort"))
			dest = &p->sort;
		else if (!strcmp(s, "branch"))
			dest = &p->branch;
		else if (!strcmp(s, "host"))
			dest = &p->host;
		else if (!strcmp(s, "url"))
			dest = &p->url;
		else if (!strcmp(s, "pattern"))
			dest = &p->regex;
		else if (!strcmp(s, "include_regex"))
			dest = &p->include;
		else if (!strcmp(s, "exclude_regex"))
			dest = &p->exclude;
		else if (!strcmp(s, "prefix"))
			dest = &p->prefix;
		else if (!strcmp(s, "token_env"))
			dest = &p->token_env;
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
			fprintf(stderr, "nvc: %s: duplicate section: %s\n", path,
				cfg->by_name[i]->name);
			return -1;
		}
	}
	for (size_t i = 0; i < cfg->len; i++) {
		p = &cfg->items[i];
		p->type = source_lookup(p->source);
		if (!p->type)
			goto invalid;
		bool github = p->type->provider == GITHUB;
		bool gitlab = p->type->provider == GITLAB;
		bool regex = p->type->operation == EXTRACT;
		if ((p->sort && (p->type->operation != TAGS || strcmp(p->sort, "numeric"))) ||
		    ((p->type->operation == COMMIT) != (p->branch != NULL)) ||
		    (p->branch && (p->prefix || !strcmp(p->branch, ".") || !strcmp(p->branch, ".."))) ||
		    (regex ? (!p->url || !p->regex || p->repo || p->host || p->token_env)
			   : (p->url || p->regex || (github ? p->host != NULL : !p->host) ||
			      !repo_valid(p->repo, gitlab) || (gitlab && strstr(p->repo, "..")))))
			goto invalid;
		if (p->host || p->url) {
			const char *value = p->host ? p->host : p->url;
			if (strncmp(value, "https://", 8) || value[8] == '/' || strchr(value, '#') ||
			    (p->host && strpbrk(value + 8, "/?")))
				goto invalid;
			CURLU *u = curl_url();
			if (!u) {
				fprintf(stderr, "nvc: %s: cannot allocate URL handle\n", p->name);
				return -1;
			}
			CURLUcode result = curl_url_set(u, CURLUPART_URL, value, CURLU_DISALLOW_USER);
			curl_url_cleanup(u);
			if (result != CURLUE_OK)
				goto invalid;
		}
		if (p->token_env) {
			if (!isalpha((unsigned char)p->token_env[0]) && p->token_env[0] != '_')
				goto invalid;
			for (const char *r = p->token_env; *r; r++)
				if (!isalnum((unsigned char)*r) && *r != '_')
					goto invalid;
		}
		if (p->regex) {
			if (regcomp(&p->extract_re, p->regex, REG_EXTENDED))
				goto invalid;
			p->extract_ready = true;
			if (p->extract_re.re_nsub < 1)
				goto invalid;
		}
		if (p->include) {
			if (regcomp(&p->include_re, p->include, REG_EXTENDED | REG_NOSUB))
				goto invalid;
			p->include_ready = true;
		}
		if (p->exclude) {
			if (regcomp(&p->exclude_re, p->exclude, REG_EXTENDED | REG_NOSUB))
				goto invalid;
			p->exclude_ready = true;
		}
		continue;
	invalid:
		fprintf(stderr, "nvc: %s: invalid source configuration\n", p->name);
		return -1;
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

static bool has_next_relation(const char *s, const char *end, bool quoted)
{
	size_t matched = 0;
	for (; s < end; s++) {
		if (quoted && *s == '\\')
			s++;
		unsigned char c = (unsigned char)*s;
		if (c == ' ' || c == '\t') {
			if (matched == 4)
				return true;
			matched = 0;
		} else if (matched < 4 && tolower(c) == "next"[matched]) {
			matched++;
		} else {
			matched = 5;
		}
	}
	return matched == 4;
}

static bool link_has_next(const char *s)
{
	while (*(s += strspn(s, ", \t"))) {
		if (*s++ != '<' || !(s = strchr(s, '>')))
			return false;
		s++;
		bool rel_seen = false, next = false, anchored = false;
		while (*(s += strspn(s, " \t")) == ';') {
			s++;
			s += strspn(s, " \t");
			const char *name = s;
			size_t len = strcspn(s, "=;, \t");
			if (!len)
				return false;
			s += len;
			bool rel = len == 3 && !strncasecmp(name, "rel", 3);
			anchored |= len == 6 && !strncasecmp(name, "anchor", 6);
			s += strspn(s, " \t");
			const char *value = s, *end = s;
			bool quoted = false;
			if (*s == '=') {
				s++;
				s += strspn(s, " \t");
				quoted = *s == '"';
				if (quoted) {
					value = ++s;
					while (*s && *s != '"') {
						if (*s++ == '\\' && *s)
							s++;
					}
					end = s;
					if (*s++ != '"')
						return false;
				} else {
					value = s;
					s += strcspn(s, ";, \t");
					end = s;
					if (value == end)
						return false;
				}
			}
			if (rel && !rel_seen)
				next = has_next_relation(value, end, quoted);
			rel_seen |= rel;
		}
		if (*s && *s != ',')
			return false;
		if (next && !anchored)
			return true;
	}
	return false;
}

static bool page_number(const char *s, size_t *number)
{
	if (*s < '0' || *s > '9')
		return false;
	size_t value = 0;
	for (; *s >= '0' && *s <= '9'; s++) {
		unsigned digit = (unsigned)(*s - '0');
		value = value > (SIZE_MAX - digit) / 10 ? SIZE_MAX : value * 10 + digit;
	}
	*number = value;
	return !*s;
}

static const char *pagination(struct project *p, size_t count)
{
	struct curl_header *h;
	CURLHcode code = curl_easy_header(p->easy, "Link", 0, CURLH_HEADER, -1, &h);
	if (code == CURLHE_NOT_BUILT_IN)
		return "libcurl header API unavailable";
	if (code != CURLHE_OK && code != CURLHE_MISSING && code != CURLHE_NOHEADERS)
		return "cannot read pagination headers";

	bool next = count == PAGE_SIZE, has_total = false;
	size_t total = 0;
	h = NULL;
	while ((h = curl_easy_nextheader(p->easy, CURLH_HEADER, -1, h))) {
		size_t n;
		if (!strcasecmp(h->name, "Link")) {
			next |= link_has_next(h->value);
		} else if (!strcasecmp(h->name, "X-Next-Page")) {
			if (page_number(h->value, &n) && n)
				next = true;
		} else if ((!strcasecmp(h->name, "X-Total-Count") ||
			    (p->type->provider == GITLAB && !strcasecmp(h->name, "X-Total"))) &&
			   page_number(h->value, &n)) {
			if (n > total)
				total = n;
			has_total = true;
		}
	}
	if (has_total)
		p->items_total = total;
	p->items_seen += count;
	if (!next && p->items_seen >= p->items_total)
		return NULL;
	if (!count)
		return "empty page before pagination ended";
	if (p->page == PAGE_MAX)
		return "pagination exceeds 100 pages";
	p->more = true;
	return NULL;
}

static size_t header_read(char *data, size_t size, size_t count, void *arg)
{
	struct project *p = arg;
	(void)data;
	if (size && count > SIZE_MAX / size)
		return 0;
	size_t n = size * count;
	if (n > 8192) {
		snprintf(p->error, sizeof(p->error), "response header exceeds 8 KiB");
		return 0;
	}
	return n;
}

#include "source.inc"

static int request_start(CURLM *multi, struct project *p, const char *test_base,
			 long timeout, bool http2)
{
	char *url, *escaped;
	const char *origin = test_base ? test_base : p->host ? p->host : "https://api.github.com";
	size_t len = strlen(origin) + (p->repo ? strlen(p->repo) * 3 : 0) +
		     (p->branch ? strlen(p->branch) * 3 : 0) + (p->url ? strlen(p->url) : 0) + 160;
	CURLcode code = CURLE_OK;
	CURLMcode mc;
	p->len = 0;
	if (p->body)
		p->body[0] = 0;
	p->more = false;
	p->page++;
	url = malloc(len);
	p->easy = curl_easy_init();
	if (!url || !p->easy) {
		free(url);
		snprintf(p->error, sizeof(p->error), "out of memory");
		goto fail;
	}
	if (p->url) {
		if (test_base) {
			const char *path = strpbrk(p->url + 8, "/?");
			snprintf(url, len, "%s%s%s", origin, path && *path == '/' ? "" : "/",
				 path ? path : "");
		} else {
			snprintf(url, len, "%s", p->url);
		}
	} else if (p->type->provider == GITLAB) {
		escaped = curl_easy_escape(p->easy, p->repo, 0);
		char *branch = p->branch ? curl_easy_escape(p->easy, p->branch, 0) : NULL;
		if (!escaped || (p->branch && !branch)) {
			curl_free(escaped);
			curl_free(branch);
			free(url);
			goto fail;
		}
		if (p->type->operation == COMMIT)
			snprintf(url, len, "%s/api/v4/projects/%s/repository/branches/%s",
				 origin, escaped, branch);
		else {
			bool tags = p->type->operation == TAGS;
			snprintf(url, len,
				 "%s/api/v4/projects/%s/%s?order_by=%s&sort=desc&per_page=%u&page=%u",
				 origin, escaped, tags ? "repository/tags" : "releases",
				 tags ? "updated" : "released_at", PAGE_SIZE, p->page);
		}
		curl_free(escaped);
		curl_free(branch);
	} else if (p->type->operation == COMMIT) {
		escaped = curl_easy_escape(p->easy, p->branch, 0);
		if (!escaped) {
			free(url);
			goto fail;
		}
		snprintf(url, len, "%s%s/repos/%s/branches/%s", origin,
			 p->type->provider == GITHUB ? "" : "/api/v1", p->repo, escaped);
		curl_free(escaped);
	} else if (p->type->operation == TAGS) {
		bool github = p->type->provider == GITHUB;
		snprintf(url, len, "%s%s/repos/%s/tags?%s=%u&page=%u", origin,
			 github ? "" : "/api/v1", p->repo, github ? "per_page" : "limit",
			 PAGE_SIZE, p->page);
	} else if (p->type->provider == GITHUB) {
		snprintf(url, len, "%s/repos/%s/releases/latest", origin, p->repo);
	} else {
		snprintf(url, len,
			 "%s/api/v1/repos/%s/releases?draft=false&pre-release=false&limit=%u&page=%u",
			 origin, p->repo, PAGE_SIZE, p->page);
	}
#define SETOPT(option, value)                                                                      \
	do {                                                                                       \
		code = curl_easy_setopt(p->easy, option, value);                                   \
		if (code != CURLE_OK)                                                              \
			goto setup_fail;                                                           \
	} while (0)
	SETOPT(CURLOPT_URL, url);
	SETOPT(CURLOPT_HTTPHEADER, p->headers);
	SETOPT(CURLOPT_USERAGENT, "nvc/" VERSION);
	SETOPT(CURLOPT_WRITEFUNCTION, body_write);
	SETOPT(CURLOPT_WRITEDATA, (void *)p);
	SETOPT(CURLOPT_HEADERFUNCTION, header_read);
	SETOPT(CURLOPT_HEADERDATA, (void *)p);
	SETOPT(CURLOPT_PRIVATE, (void *)p);
	SETOPT(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
	SETOPT(CURLOPT_TIMEOUT_MS, timeout);
	SETOPT(CURLOPT_NOSIGNAL, 1L);
	SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
	SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
	SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
	SETOPT(CURLOPT_PROTOCOLS_STR, test_base ? "http" : "https");
	if (test_base)
		SETOPT(CURLOPT_PROXY, "");
	if (http2)
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
	if (!p->error[0])
		snprintf(p->error, sizeof(p->error), "cannot encode request path");
	if (p->easy)
		curl_easy_cleanup(p->easy);
	p->easy = NULL;
	return -1;
}

static int fetch(struct config *cfg, unsigned jobs)
{
	CURLM *multi = NULL;
	const char *test_base = NULL;
	long timeout = 30000;
	size_t cursor = 0;
	unsigned active = 0;
	int status = -1;
	CURLMcode mc = CURLM_OK;
	const char *error = "cannot allocate HTTP resources";
	const char *token = NULL;

#ifdef NVC_TEST
	const char *test_timeout = getenv("NVC_TEST_TIMEOUT_MS");
	const char *loopback = "http://127.0.0.1:";
	size_t n, prefix_len = strlen(loopback);
	test_base = getenv("NVC_TEST_URL");
	if (!test_base || strncmp(test_base, loopback, prefix_len) ||
	    !page_number(test_base + prefix_len, &n) || !n || n > 65535) {
		fprintf(stderr, "nvc: fixture requires a loopback HTTP origin\n");
		return -1;
	}
	if (test_timeout) {
		if (!page_number(test_timeout, &n) || !n || n > (size_t)timeout) {
			fprintf(stderr, "nvc: invalid fixture timeout\n");
			return -1;
		}
		timeout = (long)n;
	}
#endif
	bool http2 = (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2) != 0;
	multi = curl_multi_init();
	if (!multi)
		goto done;
	for (size_t i = 0; i < cfg->len; i++) {
		struct project *p = &cfg->items[i];
		if (!p->selected)
			continue;
		const char *secret = NULL;
		if (p->token_env) {
			secret = getenv(p->token_env);
		} else if (p->type->provider == GITHUB) {
			if (!token) {
				token = getenv("GITHUB_TOKEN");
				if (!token)
					token = "";
			}
			secret = token;
		}
		p->headers = curl_slist_append(NULL, "Accept: application/json");
		if (!p->headers)
			goto done;
		if (!secret || !*secret)
			continue;
		for (const unsigned char *r = (const unsigned char *)secret; *r; r++)
			if (*r <= 32 || *r >= 127) {
				snprintf(p->error, sizeof(p->error), "invalid authentication token");
				break;
			}
		if (p->error[0])
			continue;
		size_t len = strlen(secret) + 32;
		char *auth = malloc(len);
		if (!auth)
			goto done;
		snprintf(auth, len, "%s%s",
			 (p->type->provider == GITLAB) ? "PRIVATE-TOKEN: " : "Authorization: Bearer ",
			 secret);
		struct curl_slist *next = curl_slist_append(p->headers, auth);
		free(auth);
		if (!next)
			goto done;
		p->headers = next;
	}
	while (cursor < cfg->len || active) {
		while (cursor < cfg->len && active < jobs) {
			struct project *p = &cfg->items[cursor++];
			if (p->selected && !p->error[0] &&
			    !request_start(multi, p, test_base, timeout, http2))
				active++;
		}
		int running, pending;
		mc = curl_multi_perform(multi, &running);
		if (mc != CURLM_OK)
			goto done;
		CURLMsg *msg;
		while ((msg = curl_multi_info_read(multi, &pending))) {
			char *owner;
			if (msg->msg != CURLMSG_DONE)
				continue;
			if (curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &owner) != CURLE_OK) {
				error = "cannot identify completed request";
				goto done;
			}
			struct project *p = (struct project *)owner;
			long http;
			if (msg->data.result != CURLE_OK) {
				if (!p->error[0])
					snprintf(p->error, sizeof(p->error), "request: %s",
						 curl_easy_strerror(msg->data.result));
			} else if (curl_easy_getinfo(p->easy, CURLINFO_RESPONSE_CODE, &http) != CURLE_OK) {
				snprintf(p->error, sizeof(p->error), "cannot read HTTP status");
			} else if (http != 200) {
				snprintf(p->error, sizeof(p->error), "HTTP %ld%s", http,
					 http == 404   ? ": source item missing or repository not accessible"
					 : http == 403 ? ": rate limit or permission denied"
					 : http == 429 ? ": rate limit exceeded"
						       : "");
			} else {
				source_parse(p);
			}
			mc = curl_multi_remove_handle(multi, p->easy);
			if (mc != CURLM_OK)
				goto done;
			curl_easy_cleanup(p->easy);
			p->easy = NULL;
			if (p->more && !request_start(multi, p, test_base, timeout, http2))
				continue;
			active--;
			free(p->body);
			p->body = NULL;
			p->capacity = 0;
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
		fprintf(stderr, "nvc: %s\n", mc != CURLM_OK ? curl_multi_strerror(mc) : error);
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
	return status;
}

#include "state.inc"

static void usage(FILE *out)
{
	fputs("Usage: nvc -c FILE [-j N] [PROJECT ...]\n"
	      "       nvc check -c FILE -s STATE [-j N] [PROJECT ...]\n"
	      "       nvc compare -c FILE -s STATE [PROJECT ...]\n"
	      "       nvc accept -c FILE -s STATE [PROJECT ...]\n\n"
	      "  -c FILE       INI configuration file\n"
	      "  -s STATE      State file\n"
	      "  -j N          Concurrent requests, 1-64 (default: 8)\n"
	      "  -h, --help    Show help\n"
	      "  -V, --version Show version\n",
	      out);
}

int main(int argc, char **argv)
{
	struct config cfg = {0};
	const char *path = NULL, *state = NULL, *command = NULL;
	if (argc > 1 && (!strcmp(argv[1], "check") || !strcmp(argv[1], "compare") ||
			 !strcmp(argv[1], "accept"))) {
		command = argv[1];
		argv++;
		argc--;
	}
	unsigned jobs = 8;
	int opt, status = 1;
	static const struct option options[] = {{"help", no_argument, NULL, 'h'},
						{"version", no_argument, NULL, 'V'},
						{NULL, 0, NULL, 0}};

	opterr = 0;
	while ((opt = getopt_long(argc, argv, "c:s:j:hV", options, NULL)) != -1) {
		switch (opt) {
		case 's':
			state = optarg;
			break;
		case 'c':
			path = optarg;
			break;
		case 'j': {
			if (command && strcmp(command, "check")) {
				fprintf(stderr, "nvc: -j requires a network check\n");
				return 1;
			}
			char *end;
			long n;
			errno = 0;
			n = strtol(optarg, &end, 10);
			if (!*optarg || strspn(optarg, "0123456789") != strlen(optarg) || *end ||
			    errno || n < 1 || n > 64) {
				fprintf(stderr, "nvc: concurrency must be 1-64\n");
				return 1;
			}
			jobs = (unsigned)n;
			break;
		}
		case 'h':
		case 'V':
			if (opt == 'h') {
				usage(stdout);
			} else {
#ifdef NVC_TEST
				puts("nvc-test " VERSION);
#else
				puts("nvc " VERSION);
#endif
			}
			if (fflush(stdout) || ferror(stdout)) {
				fprintf(stderr, "nvc: cannot write stdout\n");
				return 1;
			}
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}
	if (!path || !*path || (!!command != !!state) || (state && !*state)) {
		usage(stderr);
		return 1;
	}
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		fprintf(stderr, "nvc: cannot initialize curl\n");
		return 1;
	}
	if (config_read(&cfg, path))
		goto done;
	for (size_t i = 0; i < cfg.len; i++)
		cfg.items[i].selected = optind == argc;
	for (int i = optind; i < argc; i++) {
		struct project key = {.name = argv[i]}, *key_ptr = &key;
		struct project **found =
		    bsearch(&key_ptr, cfg.by_name, cfg.len, sizeof(*cfg.by_name), project_compare);
		if (!found) {
			fprintf(stderr, "nvc: unknown project: %s\n", argv[i]);
			goto done;
		}
		(*found)->selected = true;
	}
	if (command && strcmp(command, "check")) {
		status = state_run(&cfg, state, command);
		goto done;
	}
	if (fetch(&cfg, jobs)) {
		if (command) {
			for (size_t i = 0; i < cfg.len; i++)
				if (cfg.items[i].selected)
					snprintf(cfg.items[i].error, sizeof(cfg.items[i].error),
						 "check aborted");
			state_run(&cfg, state, command);
		}
		goto done;
	}
	status = command ? state_run(&cfg, state, command) : 0;
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
	curl_global_cleanup();
	return status;
}
