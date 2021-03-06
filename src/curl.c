#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <yajl/yajl_tree.h>
#include "init.h"
#include "irc.h"
#include "curl.h"
#include "common.h"

static pthread_mutex_t *openssl_mtx;

STATIC void openssl_lock_cb(int mode, int n, const char *file, int line) {

	(void) file;
	(void) line;

	if (mode & CRYPTO_LOCK)
		pthread_mutex_lock(&openssl_mtx[n]);
	else
		pthread_mutex_unlock(&openssl_mtx[n]);
}

bool openssl_crypto_init(void) {

	openssl_mtx = malloc_w(CRYPTO_num_locks() * sizeof(*openssl_mtx));

	for (int i = 0; i < CRYPTO_num_locks(); i++)
		if (pthread_mutex_init(&openssl_mtx[i], NULL))
			return false;

	CRYPTO_set_locking_callback(openssl_lock_cb);
	return true;
}

void openssl_crypto_cleanup(void) {

	CRYPTO_set_locking_callback(NULL);

	for (int i = 0; i < CRYPTO_num_locks(); i++)
		pthread_mutex_destroy(&openssl_mtx[i]);

	free(openssl_mtx);
}

size_t curl_write_memory(char *data, size_t size, size_t elements, void *membuf) {

	struct mem_buffer *mem = membuf;
	size_t total_size = size * elements;

	// Our function will be called as many times as needed by curl_easy_perform to complete the operation so,
	// we increase the size of our buffer each time to accommodate for it (and the null char)
	mem->buffer = realloc_w(mem->buffer, mem->size + total_size + 1);

	// Our mem_buffer struct keeps the current size so far, so we begin writing to the end of it each time
	memcpy(&(mem->buffer[mem->size]), data, total_size);
	mem->size += total_size;
	mem->buffer[mem->size] = '\0';

	// if the return value isn't the total number of bytes that was passed in our function,
	// curl_easy_perform will return an error
	return total_size;
}

void *shorten_url(void *long_url_arg) {

	CURL *curl;
	CURLcode code;
	char url_formatted[URLLEN], API_URL[URLLEN], *short_url = NULL;
	struct mem_buffer mem = {NULL, 0};
	struct curl_slist *headers = NULL;
	const char *long_url = long_url_arg;

	if (!*cfg.google_shortener_api_key)
		return NULL;

	curl = curl_easy_init();
	if (!curl)
		goto cleanup;

	// Set the Content-type and url format as required by Google API for the POST request
	headers = curl_slist_append(headers, "Content-Type: application/json");
	snprintf(url_formatted, URLLEN, "{\"longUrl\": \"%s\"}", long_url);

	// Include our API key in the URL
	snprintf(API_URL, URLLEN, "https://www.googleapis.com/urlshortener/v1/url?key=%s", cfg.google_shortener_api_key);

#ifdef TEST
	curl_easy_setopt(curl, CURLOPT_URL, getenv("IRCBOT_TESTFILE"));
#else
	curl_easy_setopt(curl, CURLOPT_URL, API_URL); // Set API url
#endif
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, url_formatted); // Send the formatted POST
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Allow redirects
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L); // Don't wait for too long
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // Required for use with threads. DNS queries will not honor timeout
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers); // Use our modified header

	// By default curl_easy_perform outputs the result in stdout.
	// We provide our own function, in order to save the output in a string
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);

	code = curl_easy_perform(curl); // Do the job!
	if (code != CURLE_OK) {
		fprintf(stderr, "Error: %s\n", curl_easy_strerror(code));
		goto cleanup;
	}
	if (!mem.buffer) {
		fprintf(stderr, "Error: Body was empty");
		goto cleanup;
	}
	// Find the short url in the reply and null terminate it
	short_url = strstr(mem.buffer, "http");
	if (!null_terminate(short_url, '"')) {
		short_url = NULL;
		goto cleanup;
	}
	// short_url must be freed to avoid memory leak
	short_url = strndup(short_url, ADDRLEN);

cleanup:
	free(mem.buffer);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return short_url;
}

struct github *fetch_github_commits(yajl_val *root, const char *repo, int *commit_count) {

	CURL *curl;
	CURLcode code;
	yajl_val val;
	struct github *commits = NULL;
	struct mem_buffer mem = {NULL, 0};
	char API_URL[URLLEN], errbuf[1024];

	// Use per_page field to limit json reply to the amount of commits specified
	snprintf(API_URL, URLLEN, "https://api.github.com/repos/%s/commits?per_page=%d", repo, *commit_count);
	*commit_count = 0;

	curl = curl_easy_init();
	if (!curl)
		goto cleanup;

#ifdef TEST
	curl_easy_setopt(curl, CURLOPT_URL, getenv("IRCBOT_TESTFILE"));
#else
	curl_easy_setopt(curl, CURLOPT_URL, API_URL);
#endif
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "irc-bot"); // Github requires a user-agent
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);

	code = curl_easy_perform(curl);
	if (code != CURLE_OK) {
		fprintf(stderr, "Error: %s\n", curl_easy_strerror(code));
		goto cleanup;
	}
	if (!mem.buffer) {
		fprintf(stderr, "Error: Body was empty");
		goto cleanup;
	}
	*root = yajl_tree_parse(mem.buffer, errbuf, sizeof(errbuf));
	if (!*root) {
		fprintf(stderr, "%s\n", errbuf);
		goto cleanup;
	}
	if (YAJL_IS_ARRAY(*root)) {
		*commit_count = YAJL_GET_ARRAY(*root)->len;
		commits = malloc_w(*commit_count * sizeof(*commits));
	}
	// Find the field we are interested in the json reply, save a reference to it & null terminate
	for (int i = 0; i < *commit_count; i++) {
		val = yajl_tree_get(YAJL_GET_ARRAY(*root)->values[i], CFG("sha"),                      yajl_t_string);
		if (!val) break;
		commits[i].sha  = YAJL_GET_STRING(val);

		val = yajl_tree_get(YAJL_GET_ARRAY(*root)->values[i], CFG("commit", "author", "name"), yajl_t_string);
		if (!val) break;
		commits[i].name = YAJL_GET_STRING(val);

		val = yajl_tree_get(YAJL_GET_ARRAY(*root)->values[i], CFG("commit", "message"),        yajl_t_string);
		if (!val) break;
		commits[i].msg  = YAJL_GET_STRING(val);
		null_terminate(commits[i].msg, '\n'); // Cut commit message at newline character if present

		val = yajl_tree_get(YAJL_GET_ARRAY(*root)->values[i], CFG("html_url"),                 yajl_t_string);
		if (!val) break;
		commits[i].url  = YAJL_GET_STRING(val);
	}
cleanup:
	free(mem.buffer);
	curl_easy_cleanup(curl);
	return commits;
}

char *get_url_title(const char *url) {

	CURL *curl;
	CURLcode code;
	struct mem_buffer mem = {NULL, 0};
	char *temp, *url_title = NULL;
	bool iso = false;

	curl = curl_easy_init();
	if (!curl)
		goto cleanup;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);

	code = curl_easy_perform(curl);
	if (code != CURLE_OK) {
		fprintf(stderr, "Error: %s\n", curl_easy_strerror(code));
		goto cleanup;
	}
	if (!mem.buffer) {
		fprintf(stderr, "Error: Body was empty");
		goto cleanup;
	}
	// Search http response in order to determine text encoding
	temp = strcasestr(mem.buffer, "iso-8859-7");
	if (temp) // strlen("charset") = 7
		if (starts_case_with(temp - 8, "charset") || starts_case_with(temp - 7, "charset"))
			iso = true;

	temp = strcasestr(mem.buffer, "<title");
	if (!temp)
		goto cleanup;

	url_title = temp + 7; // Point url_title to the first title character

	temp = strcasestr(url_title, "</title");
	if (!temp) {
		url_title = NULL;
		goto cleanup;
	}
	*temp = '\0'; // Terminate title string

	// Replace newline characters with spaces
	for (temp = url_title; *temp != '\0'; temp++)
		if (*temp == '\n')
			*temp = ' ';

	// If title string uses ISO 8859-7 encoding then convert it to UTF-8
	// Return value must be freed to avoid memory leak
	if (iso)
		url_title = iso8859_7_to_utf8(url_title);
	else
		url_title = strndup(url_title, TITLELEN);

cleanup:
	free(mem.buffer);
	curl_easy_cleanup(curl);
	return url_title;
}

bool fit_status(void) {

	CURL *curl;
	CURLcode code;
	struct mem_buffer mem = {NULL, 0};
	bool fitness = false;

	curl = curl_easy_init();
	if (!curl)
		goto cleanup;

	curl_easy_setopt(curl, CURLOPT_URL, "http://is.freestyler.fit.yet.charkost.gr/");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);

	code = curl_easy_perform(curl);
	if (code != CURLE_OK) {
		fprintf(stderr, "Error: %s\n", curl_easy_strerror(code));
		goto cleanup;
	}
	if (!mem.buffer) {
		fprintf(stderr, "Error: Body was empty");
		goto cleanup;
	}
	fitness = starts_with(mem.buffer, "true");

cleanup:
	free(mem.buffer);
	curl_easy_cleanup(curl);
	return fitness;
}
