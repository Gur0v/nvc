static bool state_json_scan(const char **cursor, unsigned depth)
{
	const char *p = *cursor;
	while (*p && strchr(" \t\r\n", *p))
		p++;
	if (depth > 64)
		return false;
	if (*p == '{' || *p == '[') {
		bool object = *p++ == '{';
		char end = object ? '}' : ']';
		struct json_object *keys = json_object_new_object();
		if (!keys)
			return false;
		while (*p && strchr(" \t\r\n", *p))
			p++;
		while (*p != end) {
			if (object) {
				const char *start = p;
				if (*p != '"' || !state_json_scan(&p, depth + 1))
					goto bad;
				struct json_object *key = parse_json(start, (size_t)(p - start));
				if (!key || !json_object_is_type(key, json_type_string)) {
					json_object_put(key);
					goto bad;
				}
				const char *name = json_object_get_string(key);
				struct json_object *existing;
				if (controls(name, (size_t)json_object_get_string_len(key)) ||
				    json_object_object_get_ex(keys, name, &existing)) {
					json_object_put(key);
					goto bad;
				}
				if (json_object_object_add(keys, name, key)) {
					json_object_put(key);
					goto bad;
				}
				while (*p && strchr(" \t\r\n", *p))
					p++;
				if (*p++ != ':')
					goto bad;
			}
			if (!state_json_scan(&p, depth + 1))
				goto bad;
			while (*p && strchr(" \t\r\n", *p))
				p++;
			if (*p == end)
				break;
			if (*p++ != ',')
				goto bad;
			while (*p && strchr(" \t\r\n", *p))
				p++;
		}
		json_object_put(keys);
		*cursor = p + 1;
		return true;
	bad:
		json_object_put(keys);
		return false;
	}
	if (*p == '"') {
		p++;
		while (*p && *p != '"') {
			if (*p++ == '\\') {
				if (!*p)
					return false;
				p++;
			}
		}
		if (*p++ != '"')
			return false;
	} else {
		const char *start = p;
		while (*p && !strchr(",]} \t\r\n", *p))
			p++;
		if (p == start)
			return false;
	}
	*cursor = p;
	return true;
}

static struct json_object *identity(struct project *p)
{
	struct json_object *id = json_object_new_array();
	const char *values[] = {p->source,  p->repo,	p->host,   p->url,	p->regex,
				p->include, p->exclude, p->prefix, p->token_env, p->branch, p->sort};
	if (!id)
		return NULL;
	for (size_t i = 0; i < (p->sort ? 11U : p->branch ? 10U : 9U); i++) {
		struct json_object *v = values[i] ? json_object_new_string(values[i]) : NULL;
		if ((values[i] && !v) || json_object_array_add(id, v)) {
			json_object_put(v);
			json_object_put(id);
			return NULL;
		}
	}
	return id;
}

static bool state_string(struct json_object *v)
{
	return json_object_is_type(v, json_type_null) ||
	       (json_object_is_type(v, json_type_string) && json_object_get_string_len(v) > 0 &&
		!controls(json_object_get_string(v), (size_t)json_object_get_string_len(v)) &&
		utf8_valid((const unsigned char *)json_object_get_string(v),
			   (size_t)json_object_get_string_len(v)));
}

static struct json_object *state_read(const char *path)
{
	FILE *f = fopen(path, "r");
	struct json_object *root = NULL, *format, *entries;
	if (!f) {
		if (errno != ENOENT)
			return NULL;
		root = json_object_new_object();
		if (!root)
			return NULL;
		struct json_object *version = json_object_new_int(1);
		struct json_object *items = json_object_new_object();
		if (!version || !items) {
			json_object_put(version);
			json_object_put(items);
			json_object_put(root);
			return NULL;
		}
		if (json_object_object_add(root, "format", version)) {
			json_object_put(version);
			json_object_put(items);
			json_object_put(root);
			return NULL;
		}
		if (json_object_object_add(root, "entries", items)) {
			json_object_put(items);
			json_object_put(root);
			return NULL;
		}
		return root;
	}
	char *body = malloc(CONFIG_MAX + 1);
	if (!body) {
		fclose(f);
		return NULL;
	}
	size_t n = fread(body, 1, CONFIG_MAX + 1, f);
	bool valid = !ferror(f) && n <= CONFIG_MAX;
	if (fclose(f))
		valid = false;
	if (valid) {
		body[n] = 0;
		const char *cursor = body;
		root = parse_json(body, n);
		if (root && !state_json_scan(&cursor, 0)) {
			json_object_put(root);
			root = NULL;
		}
	}
	free(body);
	if (!root || !json_object_is_type(root, json_type_object) ||
	    json_object_object_length(root) != 2 ||
	    !json_object_object_get_ex(root, "format", &format) ||
	    !json_object_is_type(format, json_type_int) || json_object_get_int64(format) != 1 ||
	    !json_object_object_get_ex(root, "entries", &entries) ||
	    !json_object_is_type(entries, json_type_object))
		goto bad;
	json_object_object_foreach(entries, name, entry)
	{
		struct json_object *id, *accepted, *pending, *eligible;
		if (!*name || controls(name, strlen(name)) ||
		    !json_object_is_type(entry, json_type_object) ||
		    json_object_object_length(entry) != 4 ||
		    !json_object_object_get_ex(entry, "identity", &id) ||
		    !json_object_is_type(id, json_type_array) ||
		    (json_object_array_length(id) != 9 && json_object_array_length(id) != 10 &&
		     json_object_array_length(id) != 11) ||
		    !json_object_object_get_ex(entry, "accepted", &accepted) ||
		    !state_string(accepted) ||
		    !json_object_object_get_ex(entry, "pending", &pending) ||
		    !state_string(pending) ||
		    !json_object_object_get_ex(entry, "eligible", &eligible) ||
		    !json_object_is_type(eligible, json_type_boolean) ||
		    (json_object_get_boolean(eligible) && !pending))
			goto bad;
		if ((json_object_array_length(id) == 10 &&
		     !json_object_is_type(json_object_array_get_idx(id, 9), json_type_string)) ||
		    !json_object_is_type(json_object_array_get_idx(id, 0), json_type_string))
			goto bad;
		if (json_object_array_length(id) == 11) {
			const char *source = json_object_get_string(json_object_array_get_idx(id, 0));
			struct json_object *sort = json_object_array_get_idx(id, 10);
			if (json_object_array_get_idx(id, 9) ||
			    (strcmp(source, "github-tags") && strcmp(source, "gitlab-tags") &&
			     strcmp(source, "forgejo-tags") && strcmp(source, "gitea-tags")) ||
			    !json_object_is_type(sort, json_type_string) ||
			    strcmp(json_object_get_string(sort), "numeric"))
				goto bad;
		}
		for (size_t i = 0; i < json_object_array_length(id); i++)
			if (!state_string(json_object_array_get_idx(id, i)))
				goto bad;
	}
	return root;
bad:
	json_object_put(root);
	return NULL;
}

static int state_write(const char *path, struct json_object *root)
{
	const char *body = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
	if (!body || strlen(body) > CONFIG_MAX) {
		errno = EFBIG;
		return -1;
	}
	char *temp = malloc(strlen(path) + 16), *directory = strdup(path);
	int fd = -1, dirfd = -1, result = -1;
	bool created = false;
	if (!temp || !directory)
		goto done;
	char *slash = strrchr(directory, '/');
	if (slash) {
		if (slash == directory)
			slash[1] = 0;
		else
			*slash = 0;
	} else
		strcpy(directory, ".");
	dirfd = open(directory, O_RDONLY | O_DIRECTORY);
	if (dirfd < 0)
		goto done;
	sprintf(temp, "%s.tmp.XXXXXX", path);
	fd = mkstemp(temp);
	if (fd < 0)
		goto done;
	created = true;
	size_t len = strlen(body), offset = 0;
	while (offset < len) {
		ssize_t n = write(fd, body + offset, len - offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto done;
		offset += (size_t)n;
	}
	if (fsync(fd))
		goto done;
	if (close(fd)) {
		fd = -1;
		goto done;
	}
	fd = -1;
#ifdef NVC_TEST
	if (getenv("NVC_TEST_STATE_INTERRUPT"))
		_exit(99);
#endif
	if (rename(temp, path) || fsync(dirfd))
		goto done;
	result = 0;
done:
	if (fd >= 0)
		close(fd);
	if (dirfd >= 0)
		close(dirfd);
	if (result && created)
		unlink(temp);
	free(temp);
	free(directory);
	return result;
}

static int state_run(struct config *cfg, const char *path, const char *command)
{
	char *lockpath = malloc(strlen(path) + 6);
	struct json_object *root = NULL, *entries;
	int fd = -1, status = 1;
	if (!lockpath)
		goto done;
	sprintf(lockpath, "%s.lock", path);
	fd = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
	if (fd < 0)
		goto done;
	struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
	while (fcntl(fd, F_SETLKW, &lock) < 0)
		if (errno != EINTR)
			goto done;
	root = state_read(path);
	if (!root) {
		fprintf(stderr, "nvc: invalid or unreadable state\n");
		goto done;
	}
	json_object_object_get_ex(root, "entries", &entries);
	status = 0;
	bool check = !strcmp(command, "check"), accept = !strcmp(command, "accept");
	for (size_t i = 0; i < cfg->len; i++) {
		struct project *p = &cfg->items[i];
		struct json_object *entry = NULL, *oldid = NULL, *accepted = NULL, *pending = NULL,
				   *eligible = NULL;
		if (!p->selected)
			continue;
		struct json_object *id = identity(p);
		if (!id) {
			status = 1;
			break;
		}
		json_object_object_get_ex(entries, p->name, &entry);
		if (entry)
			json_object_object_get_ex(entry, "identity", &oldid);
		bool same = oldid && json_object_equal(id, oldid);
		if (same) {
			json_object_object_get_ex(entry, "accepted", &accepted);
			json_object_object_get_ex(entry, "pending", &pending);
			json_object_object_get_ex(entry, "eligible", &eligible);
		}
		if (check) {
			struct json_object *next = json_object_new_object();
			if (!next) {
				json_object_put(id);
				status = 1;
				break;
			}
			struct json_object *value = p->version && !p->error[0]
							? json_object_new_string(p->version)
							: json_object_get(pending);
			struct json_object *ok =
			    json_object_new_boolean(p->version && !p->error[0]);
			if ((p->version && !p->error[0] && !value) || !ok) {
				json_object_put(value);
				json_object_put(ok);
				json_object_put(id);
				json_object_put(next);
				status = 1;
				break;
			}
			if (json_object_object_add(next, "identity", id)) {
				json_object_put(id);
				json_object_put(value);
				json_object_put(ok);
				json_object_put(next);
				status = 1;
				break;
			}
			struct json_object *a = json_object_get(accepted);
			if (json_object_object_add(next, "accepted", a)) {
				json_object_put(a);
				json_object_put(value);
				json_object_put(ok);
				json_object_put(next);
				status = 1;
				break;
			}
			if (json_object_object_add(next, "pending", value)) {
				json_object_put(value);
				json_object_put(ok);
				json_object_put(next);
				status = 1;
				break;
			}
			if (json_object_object_add(next, "eligible", ok)) {
				json_object_put(ok);
				json_object_put(next);
				status = 1;
				break;
			}
			if (json_object_object_add(entries, p->name, next)) {
				json_object_put(next);
				status = 1;
				break;
			}
			continue;
		}
		json_object_put(id);
		bool valid = same && pending && json_object_get_boolean(eligible);
		if (accept) {
			if (!valid) {
				fprintf(stderr, "nvc: %s: no eligible observation\n", p->name);
				status = 1;
			} else {
				struct json_object *value = json_object_get(pending);
				if (json_object_object_add(entry, "accepted", value)) {
					json_object_put(value);
					status = 1;
				}
			}
		} else {
			const char *a = accepted ? json_object_get_string(accepted) : "";
			const char *v = pending ? json_object_get_string(pending) : "";
			const char *kind = !same	  ? "unchecked"
					   : !valid	  ? "failed"
					   : !accepted	  ? "new"
					   : strcmp(a, v) ? "changed"
							  : "unchanged";
			if (printf("%s\t%s\t%s\t%s\n", p->name, kind, a, v) < 0)
				status = 1;
			if (!valid)
				status = 1;
		}
	}
	if ((check || accept) && !status && state_write(path, root))
		status = 1;
	if (fflush(stdout) || ferror(stdout))
		status = 1;
done:
	if (status)
		fprintf(stderr, "nvc: state operation failed\n");
	json_object_put(root);
	if (fd >= 0)
		close(fd);
	free(lockpath);
	return status;
}
