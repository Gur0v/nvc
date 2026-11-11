static int json_code_unit(const char *p)
{
	int code = 0;
	for (unsigned i = 0; i < 4; i++) {
		int c = (unsigned char)p[i], digit;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else
			return -1;
		code = code * 16 + digit;
	}
	return code;
}

static const char *json_string_end(const char *p)
{
	for (p++; *p != '"'; p++) {
		if ((unsigned char)*p < 32)
			return NULL;
		if (*p != '\\')
			continue;
		p++;
		if (!*p)
			return NULL;
		if (*p != 'u')
			continue;
		int code = json_code_unit(p + 1);
		if (code < 0 || (code >= 0xdc00 && code <= 0xdfff))
			return NULL;
		p += 4;
		if (code >= 0xd800 && code <= 0xdbff) {
			if (p[1] != '\\' || p[2] != 'u')
				return NULL;
			int low = json_code_unit(p + 3);
			if (low < 0xdc00 || low > 0xdfff)
				return NULL;
			p += 6;
		}
	}
	return p + 1;
}

static const char *json_number_end(const char *p)
{
	const char *digits = "0123456789";
	if (*p == '-')
		p++;
	if (*p == '0') {
		p++;
	} else {
		size_t n = strspn(p, digits);
		if (!n)
			return NULL;
		p += n;
	}
	if (*p == '.') {
		size_t n = strspn(++p, digits);
		if (!n)
			return NULL;
		p += n;
	}
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-')
			p++;
		size_t n = strspn(p, digits);
		if (!n)
			return NULL;
		p += n;
	}
	return p;
}

static const char *json_value_end(struct json_tokener *tok, const char *p, unsigned depth)
{
	p += strspn(p, " \t\r\n");
	if (depth > JSON_TOKENER_DEFAULT_DEPTH)
		return NULL;
	if (*p == '"')
		return json_string_end(p);
	if (!strncmp(p, "true", 4) || !strncmp(p, "null", 4))
		return p + 4;
	if (!strncmp(p, "false", 5))
		return p + 5;
	if (*p != '{' && *p != '[')
		return json_number_end(p);

	bool object = *p++ == '{';
	char end = object ? '}' : ']';
	struct json_object *keys = object ? json_object_new_object() : NULL;
	if (object && !keys)
		return NULL;
	p += strspn(p, " \t\r\n");
	while (*p != end) {
		if (object) {
			const char *start = p;
			if (*p != '"' || !(p = json_string_end(p)))
				goto bad;
			json_tokener_reset(tok);
			struct json_object *key = json_tokener_parse_ex(tok, start, (int)(p - start));
			if (!key || json_tokener_get_error(tok) != json_tokener_success) {
				json_object_put(key);
				goto bad;
			}
			const char *name = json_object_get_string(key);
			bool valid = !controls(name, (size_t)json_object_get_string_len(key)) &&
				     !json_object_object_get_ex(keys, name, NULL) &&
				     !json_object_object_add(keys, name, NULL);
			json_object_put(key);
			p += strspn(p, " \t\r\n");
			if (!valid || *p++ != ':')
				goto bad;
		}
		p = json_value_end(tok, p, depth + 1);
		if (!p)
			goto bad;
		p += strspn(p, " \t\r\n");
		if (*p == end)
			break;
		if (*p++ != ',')
			goto bad;
		p += strspn(p, " \t\r\n");
	}
	json_object_put(keys);
	return p + 1;
bad:
	json_object_put(keys);
	return NULL;
}

static struct json_object *parse_document(const char *body, size_t len)
{
	if (!body || !len || len > INT_MAX || memchr(body, 0, len) ||
	    !utf8_valid((const unsigned char *)body, len))
		return NULL;
	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		return NULL;
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	struct json_object *root = json_tokener_parse_ex(tok, body, (int)len);
	const char *end = NULL;
	if (root && json_tokener_get_error(tok) == json_tokener_success)
		end = json_value_end(tok, body, 0);
	if (!end || end + strspn(end, " \t\r\n") != body + len) {
		json_object_put(root);
		root = NULL;
	}
	json_tokener_free(tok);
	return root;
}

static bool numeric_valid(const char *s)
{
	for (;;) {
		if (*s < '0' || *s > '9')
			return false;
		while (*s >= '0' && *s <= '9')
			s++;
		if (!*s)
			return true;
		if (*s++ != '.')
			return false;
	}
}

static int numeric_compare(const char *a, const char *b)
{
	while (*a || *b) {
		while (*a == '0')
			a++;
		while (*b == '0')
			b++;
		size_t na = strspn(a, "0123456789");
		size_t nb = strspn(b, "0123456789");
		if (na != nb)
			return na > nb ? 1 : -1;
		int cmp = strncmp(a, b, na);
		if (cmp)
			return cmp;
		a += na;
		b += nb;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
	return 0;
}

static int candidate(struct project *p, const char *value, size_t len)
{
	if (!len || controls(value, len) || !utf8_valid((const unsigned char *)value, len))
		return -1;
	if (p->include_ready) {
		int rc = regexec(&p->include_re, value, 0, NULL, 0);
		if (rc)
			return rc == REG_NOMATCH ? 0 : -1;
	}
	if (p->exclude_ready) {
		int rc = regexec(&p->exclude_re, value, 0, NULL, 0);
		if (rc != REG_NOMATCH)
			return rc ? -1 : 0;
	}
	if (p->prefix && !strncmp(value, p->prefix, strlen(p->prefix)))
		value += strlen(p->prefix);
	if (!*value)
		return -1;
	if (p->sort) {
		if (!numeric_valid(value))
			return -2;
		if (p->version && numeric_compare(value, p->version) <= 0)
			return 1;
	}
	char *version = strdup(value);
	if (!version)
		return -1;
	free(p->version);
	p->version = version;
	return 1;
}

static void source_parse(struct project *p)
{
	struct json_object *root = NULL, *item, *tag, *flag;
	const char *error = "invalid source response";
	bool github = p->type->provider == GITHUB && p->type->operation == RELEASE;
	bool tags = p->type->operation == TAGS;
	bool gitlab = p->type->provider == GITLAB;
	p->more = false;
	if (p->extract_ready) {
		regmatch_t match[2];
		if (!p->body || memchr(p->body, 0, p->len) ||
		    !utf8_valid((const unsigned char *)p->body, p->len) ||
		    regexec(&p->extract_re, p->body, 2, match, 0) || match[1].rm_so < 0)
			goto done;
		p->body[match[1].rm_eo] = 0;
		int result = candidate(p, p->body + match[1].rm_so,
				       (size_t)(match[1].rm_eo - match[1].rm_so));
		if (result == 1)
			error = NULL;
		else
			error = result ? "invalid version identifier" : "no matching candidate";
		goto done;
	}
	root = parse_document(p->body, p->len);
	if (p->type->operation == COMMIT) {
		struct json_object *commit;
		if (!root || !json_object_is_type(root, json_type_object) ||
		    !json_object_object_get_ex(root, "commit", &commit) ||
		    !json_object_is_type(commit, json_type_object) ||
		    !json_object_object_get_ex(commit, p->type->provider == GITHUB ? "sha" : "id", &tag) ||
		    !json_object_is_type(tag, json_type_string))
			goto done;
		const char *id = json_object_get_string(tag);
		size_t len = (size_t)json_object_get_string_len(tag);
		if ((len != 40 && len != 64) || strspn(id, "0123456789abcdefABCDEF") != len)
			goto done;
		int result = candidate(p, id, len);
		error = result == 1 ? NULL
			: result == 0 ? "no matching candidate" : "invalid commit identifier";
		goto done;
	}
	if (!root || !json_object_is_type(root, github ? json_type_object : json_type_array))
		goto done;
	size_t count = github ? 1 : json_object_array_length(root);
	if (!github && count > PAGE_SIZE)
		goto done;
	for (size_t i = 0; i < count; i++) {
		item = github ? root : json_object_array_get_idx(root, i);
		if (!json_object_is_type(item, json_type_object))
			goto done;
		if (!tags) {
			if (gitlab) {
				if (json_object_object_get_ex(item, "upcoming_release", &flag)) {
					if (!json_object_is_type(flag, json_type_boolean))
						goto done;
					if (json_object_get_boolean(flag))
						continue;
				}
			} else {
				bool skip = false;
				const char *fields[] = {"draft", "prerelease"};
				for (size_t k = 0; k < 2; k++) {
					if (!json_object_object_get_ex(item, fields[k], &flag) ||
					    !json_object_is_type(flag, json_type_boolean))
						goto done;
					skip |= json_object_get_boolean(flag) != 0;
				}
				if (skip)
					continue;
			}
		}
		if (!json_object_object_get_ex(item, tags ? "name" : "tag_name", &tag) ||
		    !json_object_is_type(tag, json_type_string))
			goto done;
		int result = candidate(p, json_object_get_string(tag),
				       (size_t)json_object_get_string_len(tag));
		if (result < 0) {
			error = result == -2 ? "invalid numeric tag identifier" : "invalid version identifier";
			goto done;
		}
		if (result > 0 && !p->sort) {
			error = NULL;
			goto done;
		}
	}
	error = github ? NULL : pagination(p, count);
	if (!error && !p->more && !(p->sort && p->version))
		error = tags ? "no matching tag" : "no matching stable candidate";
done:
	if (error)
		snprintf(p->error, sizeof(p->error), "%s", error);
	json_object_put(root);
}
