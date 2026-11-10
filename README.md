# nvc

A minimal nv checker in C

## Description

nvc checks upstream versions from an INI configuration file. It uses concurrent
HTTP requests and writes results as tab-separated text for use in scripts.

Currently, nvc supports GitHub releases. It reports release tags unchanged,
without version sorting, normalization, or comparison with previous runs.

## Requirements

- A C17 compiler
- Make and pkg-config
- libcurl 7.85.0 or later
- json-c 0.15 or later
- Python 3, for tests only

The code requires POSIX interfaces and `getopt_long`. A working CA certificate
store is required for HTTPS requests.

## Building

```sh
make
```

The executable is written to `./nvc`. Run it from the build directory or copy it
to a directory in your `PATH`. There is no install target.

The Makefile accepts `CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS`, and
`PKG_CONFIG`. Clean the build before changing compiler options:

```sh
make clean
make CC=clang CFLAGS='-O2 -g'
```

## Usage

```text
nvc -c file [-j jobs] [project ...]
nvc -h | --help
nvc -V | --version
```

- `-c file`: Read projects from the specified INI file. This option is required
  for checks; nvc does not search for a default configuration.
- `-j jobs`: Set the maximum number of concurrent requests, from 1 through 64.
  The default is 8.
- `project ...`: Check only the named configuration sections. With no project
  arguments, check all sections. Unknown names are errors. Repeated names are
  checked once.
- `-h`, `--help`: Display usage information and exit.
- `-V`, `--version`: Display the program version and exit.

For example:

```sh
./nvc -c versions.ini
./nvc -c versions.ini -j 4 neovim
```

### Output

Each successful check writes one line to standard output, with the project name
and release tag separated by a tab. Results follow configuration order,
regardless of request completion order or the order of project arguments.

Errors are written to standard error. A failed check does not cancel other
checks; successful results are still printed.

## Configuration

Each section names a project and requires `source` and `repo`:

```ini
[curl]
source = github
repo = curl/curl

[neovim]
source = github
repo = neovim/neovim
```

`source` must be `github`. `repo` specifies `owner/name`; components may contain
ASCII letters, digits, dots, underscores, and hyphens.

Section names and keys are case-sensitive. Duplicate sections, duplicate keys,
unknown keys, unsupported sources, missing values, and keys outside sections are
errors. The entire configuration is validated before requests begin.

Blank lines and full-line comments beginning with `#` or `;` are accepted.
Surrounding spaces and tabs are trimmed from section names, keys, and values.
Both LF and CRLF line endings are accepted.

Values are literal text. There are no inline comments, quote or escape
processing, variable expansion, includes, or multiline values. The configuration
file must not exceed 1 MiB.

### GitHub releases

The GitHub source uses the API endpoint
`https://api.github.com/repos/OWNER/REPO/releases/latest`. GitHub determines which
release is latest; nvc does not select or sort repository tags.

Drafts and prereleases are rejected. Repositories without a qualifying release
produce an error, with no fallback to tags. Version identifiers retain prefixes
such as `v`. Empty identifiers, invalid UTF-8, and control characters are rejected.

Requests verify TLS certificates and hostnames. Redirects are not followed;
update `repo` if a renamed or transferred repository redirects. Each request has
a 10-second connection timeout, a 30-second total timeout, and a 1 MiB response
limit. Requests are not retried or cached.

HTTP 403 may indicate a rate limit or insufficient permissions. HTTP 429 indicates
rate limiting.

## Environment

`GITHUB_TOKEN` is an optional token used to authenticate GitHub API requests. An
unset or empty value uses anonymous access. Tokens must contain printable ASCII
without whitespace. Tokens are not read from the configuration file or printed
in diagnostics.

Standard proxy environment variables supported by libcurl apply to production
requests.

## Exit status

nvc exits with status 0 when all selected checks succeed. Help and version
requests also exit with status 0. Invalid arguments, configuration errors, failed
checks, and output errors return a nonzero status.

## Testing

```sh
make test
```

Tests use local HTTP fixtures and do not require GitHub access or credentials.
The test executable has compile-time fixture support that is not present in the
production executable.

To run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make clean
make CC=clang CFLAGS='-O1 -g -fsanitize=address,undefined' \
    LDFLAGS='-fsanitize=address,undefined' test
```

Run `make clean` before returning to a normal build.

## License

nvc is distributed under the BSD 3-Clause License. See [LICENSE](LICENSE) for the
full terms.
