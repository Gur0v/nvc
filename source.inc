static struct json_object *parse_json(const char *body, size_t len)
{
	struct json_tokener *tok = json_tokener_new();
	struct json_object *root = NULL;
	if (!tok || !body || !len || len > INT_MAX || !utf8_valid((const unsigned char *)body, len))
		goto done;
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	root = json_tokener_parse_ex(tok, body, (int)len);
	size_t end = json_tokener_get_parse_end(tok);
	while (end < len && body[end] && strchr(" \t\r\n", body[end]))
		end++;
	if (json_tokener_get_error(tok) != json_tokener_success || end != len) {
		json_object_put(root);
		root = NULL;
	}
done:
	if (tok)
		json_tokener_free(tok);
	return root;
}

static int candidate(struct project *p, const char *value, size_t len)
{
	if (!len || controls(value, len) || !utf8_valid((const unsigned char *)value, len))
		return -1;
	if ((p->include_ready && regexec(&p->include_re, value, 0, NULL, 0)) ||
	    (p->exclude_ready && !regexec(&p->exclude_re, value, 0, NULL, 0)))
		return 0;
	if (p->prefix && !strncmp(value, p->prefix, strlen(p->prefix)))
		value += strlen(p->prefix);
	if (!*value)
		return -1;
	p->version = strdup(value);
	return p->version ? 1 : -1;
}

static void source_parse(struct project *p)
{
	struct json_object *root = NULL, *item, *tag, *flag;
	const char *error = "invalid source response";
	bool github = !strcmp(p->source, "github");
	bool tags = !strcmp(p->source, "github-tags");
	bool gitlab = !strcmp(p->source, "gitlab");
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
	root = parse_json(p->body, p->len);
	if (!root || !json_object_is_type(root, github ? json_type_object : json_type_array))
		goto done;
	size_t count = github ? 1 : json_object_array_length(root);
	if (!github && count > 100)
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
			error = "invalid version identifier";
			goto done;
		}
		if (result > 0) {
			error = NULL;
			goto done;
		}
	}
	if (!github && (count == 100 || p->next_page)) {
		if (p->page >= 100)
			error = "pagination exceeds 100 pages";
		else {
			p->more = true;
			error = NULL;
		}
	} else
		error = "no matching stable candidate";
done:
	if (error)
		snprintf(p->error, sizeof(p->error), "%s", error);
	json_object_put(root);
}
