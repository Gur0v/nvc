static struct json_object *identity(const struct project *p)
{
	int len = p->sort ? 11 : p->branch ? 10 : 9;
	struct json_object *id = json_object_new_array_ext(len);
	const char *values[] = {
		p->source, p->repo, p->host, p->url, p->regex, p->include, p->exclude,
		p->prefix, p->token_env, p->branch, p->sort,
	};
	if (!id)
		return NULL;
	for (int i = 0; i < len; i++) {
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

static bool state_identity(struct json_object *id)
{
	if (!json_object_is_type(id, json_type_array))
		return false;
	size_t len = json_object_array_length(id);
	if (len < 9 || len > 11)
		return false;
	for (size_t i = 0; i < len; i++)
		if (!state_string(json_object_array_get_idx(id, i)))
			return false;
	struct json_object *name = json_object_array_get_idx(id, 0);
	if (!name)
		return false;
	const struct source_type *type = source_lookup(json_object_get_string(name));
	if (!type)
		return false;
	if (type->operation == COMMIT)
		return len == 10 && json_object_array_get_idx(id, 9);
	if (len == 9)
		return true;
	if (len != 11 || type->operation != TAGS || json_object_array_get_idx(id, 9))
		return false;
	struct json_object *sort = json_object_array_get_idx(id, 10);
	return sort && !strcmp(json_object_get_string(sort), "numeric");
}

static struct json_object *state_read(const char *path)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	struct json_object *root = NULL, *format, *entries;
	if (fd < 0) {
		if (errno != ENOENT) {
			fprintf(stderr, "nvc: %s: cannot open state file: %s\n", path, strerror(errno));
			return NULL;
		}
		static const char empty[] = "{\"format\":1,\"entries\":{}}";
		root = parse_document(empty, sizeof(empty) - 1);
		if (!root)
			fprintf(stderr, "nvc: %s: cannot initialize state\n", path);
		return root;
	}
	struct stat st;
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "nvc: %s: cannot stat state file: %s\n", path, strerror(errno));
		close(fd);
		return NULL;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "nvc: %s: state file must be a regular file\n", path);
		close(fd);
		return NULL;
	}
	FILE *f = fdopen(fd, "r");
	if (!f) {
		fprintf(stderr, "nvc: %s: cannot open state stream: %s\n", path, strerror(errno));
		close(fd);
		return NULL;
	}
	char *body = malloc(CONFIG_MAX + 1);
	if (!body) {
		fprintf(stderr, "nvc: %s: cannot allocate state buffer\n", path);
		fclose(f);
		return NULL;
	}
	size_t n = fread(body, 1, CONFIG_MAX + 1, f);
	bool valid = !ferror(f);
	if (fclose(f) || !valid || n > CONFIG_MAX) {
		fprintf(stderr, "nvc: %s: %s\n", path,
			n > CONFIG_MAX ? "state exceeds 1 MiB" : "cannot read state file");
		free(body);
		return NULL;
	}
	body[n] = 0;
	root = parse_document(body, n);
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
		if (!*name || !json_object_is_type(entry, json_type_object) ||
		    json_object_object_length(entry) != 4 ||
		    !json_object_object_get_ex(entry, "identity", &id) ||
		    !state_identity(id) ||
		    !json_object_object_get_ex(entry, "accepted", &accepted) ||
		    !state_string(accepted) ||
		    !json_object_object_get_ex(entry, "pending", &pending) ||
		    !state_string(pending) ||
		    !json_object_object_get_ex(entry, "eligible", &eligible) ||
		    !json_object_is_type(eligible, json_type_boolean) ||
		    (json_object_get_boolean(eligible) && !pending))
			goto bad;
	}
	return root;
bad:
	fprintf(stderr, "nvc: %s: invalid state format\n", path);
	json_object_put(root);
	return NULL;
}

static int state_write(const char *path, struct json_object *root)
{
	const char *body = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
	const char *operation = "serialize state";
	char *temp = NULL, *directory = NULL;
	int fd = -1, dirfd = -1, error = 0;
	bool created = false, renamed = false;
	if (!body) {
		errno = ENOMEM;
		goto failed;
	}
	size_t len = strlen(body), offset = 0;
	if (len > CONFIG_MAX) {
		errno = EFBIG;
		goto failed;
	}
	operation = "allocate state paths";
	temp = malloc(strlen(path) + 16);
	if (!temp)
		goto failed;
	directory = strdup(path);
	if (!directory)
		goto failed;
	char *slash = strrchr(directory, '/');
	if (slash) {
		if (slash == directory)
			slash[1] = 0;
		else
			*slash = 0;
	}
	operation = "open state directory";
	dirfd = open(slash ? directory : ".", O_RDONLY | O_DIRECTORY);
	if (dirfd < 0)
		goto failed;
	operation = "create temporary state file";
	sprintf(temp, "%s.tmp.XXXXXX", path);
	fd = mkstemp(temp);
	if (fd < 0)
		goto failed;
	created = true;
	operation = "write temporary state file";
	while (offset < len) {
		ssize_t n = write(fd, body + offset, len - offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n == 0)
			errno = EIO;
		if (n <= 0)
			goto failed;
		offset += (size_t)n;
	}
	operation = "sync temporary state file";
	if (fsync(fd))
		goto failed;
	operation = "close temporary state file";
	if (close(fd)) {
		fd = -1;
		goto failed;
	}
	fd = -1;
#ifdef NVC_TEST
	if (getenv("NVC_TEST_STATE_INTERRUPT"))
		_exit(99);
#endif
	operation = "replace state file";
	if (rename(temp, path))
		goto failed;
	renamed = true;
	operation = "sync state directory";
	if (fsync(dirfd))
		goto failed;
	goto done;
failed:
	error = errno;
done:
	if (fd >= 0)
		close(fd);
	if (dirfd >= 0)
		close(dirfd);
	if (created && !renamed)
		unlink(temp);
	free(temp);
	free(directory);
	if (!error)
		return 0;
	fprintf(stderr, "nvc: %s: cannot %s: %s%s\n", path, operation, strerror(error),
		renamed ? "; new state is already visible" : "");
	errno = error;
	return -1;
}

static int state_observe(struct json_object *entries, const struct project *p,
			 struct json_object *id, struct json_object *accepted,
			 struct json_object *pending)
{
	struct json_object *entry = json_object_new_object();
	if (!entry)
		return -1;
	bool success = p->version && !p->error[0];
	struct json_object *value = success ? json_object_new_string(p->version)
					    : json_object_get(pending);
	struct json_object *eligible = json_object_new_boolean(success);
	struct {
		const char *key;
		struct json_object *value;
	} fields[] = {
		{"identity", json_object_get(id)},
		{"accepted", json_object_get(accepted)},
		{"pending", value},
		{"eligible", eligible},
	};
	size_t i = 0, len = sizeof(fields) / sizeof(*fields);
	if (eligible && (!success || value)) {
		for (; i < len; i++)
			if (json_object_object_add(entry, fields[i].key, fields[i].value))
				break;
		if (i == len && !json_object_object_add(entries, p->name, entry))
			return 0;
	}
	for (; i < len; i++)
		json_object_put(fields[i].value);
	json_object_put(entry);
	return -1;
}

static int state_run(struct config *cfg, const char *path, const char *command)
{
	char *lockpath = malloc(strlen(path) + 6);
	struct json_object *root = NULL, *entries;
	const char *operation = "allocate state lock path";
	int fd = -1, status = 1;
	if (!lockpath)
		goto lock_error;
	sprintf(lockpath, "%s.lock", path);
	operation = "open state lock";
	fd = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
	if (fd < 0)
		goto lock_error;
	struct stat st;
	operation = "stat state lock";
	if (fstat(fd, &st) < 0)
		goto lock_error;
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "nvc: %s: state lock must be a regular file\n", lockpath);
		goto done;
	}
	struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
	operation = "acquire state lock";
	while (fcntl(fd, F_SETLKW, &lock) < 0)
		if (errno != EINTR)
			goto lock_error;
	root = state_read(path);
	if (!root)
		goto done;
	json_object_object_get_ex(root, "entries", &entries);
	status = 0;
	bool check = !strcmp(command, "check"), accept = !strcmp(command, "accept");
	if (!check && !accept) {
		close(fd);
		fd = -1;
	}
	for (size_t i = 0; i < cfg->len; i++) {
		struct project *p = &cfg->items[i];
		struct json_object *entry = NULL, *oldid = NULL, *accepted = NULL, *pending = NULL,
				   *eligible = NULL;
		if (!p->selected)
			continue;
		struct json_object *id = identity(p);
		if (!id)
			goto allocation_error;
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
			int rc = state_observe(entries, p, id, accepted, pending);
			json_object_put(id);
			if (rc)
				goto allocation_error;
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
					goto allocation_error;
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
				goto stdout_error;
			if (!valid) {
				fprintf(stderr, "nvc: %s: %s observation\n", p->name, kind);
				status = 1;
			}
		}
	}
	if ((check || accept) && !status && state_write(path, root))
		status = 1;
	if (!check && !accept && (fflush(stdout) || ferror(stdout)))
		goto stdout_error;
	goto done;
lock_error:
	fprintf(stderr, "nvc: %s: cannot %s: %s\n", lockpath ? lockpath : path,
		operation, strerror(errno));
	goto done;
allocation_error:
	fprintf(stderr, "nvc: %s: cannot allocate state entry\n", path);
	status = 1;
	goto done;
stdout_error:
	fprintf(stderr, "nvc: cannot write stdout\n");
	status = 1;
done:
	json_object_put(root);
	if (fd >= 0)
		close(fd);
	free(lockpath);
	return status;
}
