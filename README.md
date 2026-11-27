# vectus

A C99 client for [tus](https://tus.io), the open protocol for resumable uploads, built on libcurl.
Uploads survive dropped connections, stalled networks and restarted processes: call `vectus_upload_perform`
again and it carries on from the last byte the server has.

```c
#include <vectus/vectus.h>

vectus_client client;
vectus_upload upload;

vectus_client_create(&client, "https://tus.example.com/files/");
vectus_client_set_header(client, "Authorization", "Bearer <token>");
vectus_upload_create_file(&upload, "video.mp4");

if (vectus_upload_perform(client, upload) == VECTUS_OK) {
    printf("uploaded to %s\n", vectus_upload_url(upload));
} else {
    fprintf(stderr, "%s\n", vectus_upload_error(upload));
}

vectus_upload_cleanup(upload);
vectus_client_cleanup(client);
```

[`examples/upload.c`](examples/upload.c) is the same with error handling, retries and a progress line,
built as `vectus-upload`:

```sh
vectus-upload https://tus.example.com/files/ video.mp4
```

## Build and install

vectus needs libcurl, and CMake 3.21 or newer to build. The tests also need [Check](https://libcheck.github.io/check/).

```sh
cmake -B build
cmake --build build
ctest --test-dir build
cmake --install build
```

Then `find_package(vectus)` and link `vectus::vectus` from CMake, or use `pkg-config --cflags --libs vectus`.

## Using it

**Uploads come from a file** with `vectus_upload_create_file`, or **from anything else** through a read callback
with `vectus_upload_create`. Give it a seek callback too, so an upload can resume mid-way. When the length is not
known up front, as for a pipe, pass `VECTUS_LENGTH_UNKNOWN`, and the stream's end is the upload's end.

**Errors** come back as a `vectus_code`; `vectus_strerror` names it, and `vectus_upload_error` says what happened,
such as `PATCH answered HTTP 503` or curl's reason for a failed connection. `vectus_upload_http_status` gives
the status to act on. These are worth another `vectus_upload_perform`, which resumes where the server stopped:

- `VECTUS_ERR_TRANSPORT`: the connection failed or stalled for 30 seconds
- `VECTUS_ERR_HTTP_STATUS`: a 5xx, or a 401 once you set fresh credentials with `vectus_client_set_header`
- `VECTUS_ERR_CANCELLED`: your progress callback stopped the upload

`VECTUS_ERR_GONE` means the server no longer has the upload: forget its URL and start over.

**Resuming in another process** needs the upload's URL: store `vectus_upload_url` once it is set, and hand it
to `vectus_upload_set_url` on a new upload of the same data. `vectus_upload_expires` says when the server will
remove an unfinished one.

**Progress** reaches a callback set with `vectus_upload_set_progress`, at least once a second while a request
runs; returning non-zero cancels the upload, which can be resumed later.

**Metadata**, such as a file name, goes along with `vectus_upload_add_metadata`.

**Parallel uploads** split the data into partial uploads (`vectus_upload_set_partial`), each sent on its own
thread, then join them with `vectus_upload_create_final`.

**TLS, proxies and other curl settings** go through `vectus_client_set_curl_callback`, called with each
request's `CURL *` handle:

```c
#include <curl/curl.h>

static void use_company_ca(void *userdata, void *curl) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/company-ca.pem");
}

vectus_client_set_curl_callback(client, use_company_ca, NULL);
```

Code calling curl itself links it too: `CURL::libcurl` from `find_package(CURL)` in CMake; pkg-config already adds it.

**Threads**: call `curl_global_init(CURL_GLOBAL_DEFAULT)` once before starting them, unless your libcurl is
7.84 or newer and built thread-safe. Threads may share a client, each with its own uploads, once
`vectus_client_discover` asked the server what it offers; before that, their uploads would all ask at once.

The headers in [`include/vectus`](include/vectus) document every function.

## Protocol support

tus 1.0.0 with the creation, creation-with-upload, creation-defer-length, expiration, termination and
concatenation extensions. The checksum extension is not supported yet. Clients can send PATCH and DELETE as POST
with `X-HTTP-Method-Override`, for proxies that block them (`vectus_client_set_method_override`).

## Testing

`ctest --test-dir build` runs the unit tests, and tests against a fake server, under AddressSanitizer and
UndefinedBehaviorSanitizer. With Docker, `tests/e2e-env.sh` starts tusd, and Toxiproxy to break connections on
purpose, and prints the variables that also run the tests against them:

```sh
export $(tests/e2e-env.sh)
ctest --test-dir build --output-on-failure
```

## License

MIT
