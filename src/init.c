#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <yajl/yajl_tree.h>
#include "irc.h"
#include "init.h"
#include "socket.h"
#include "queue.h"
#include "murmur.h"
#include "curl.h"
#include "mpd.h"
#include "database.h"
#include "common.h"

// Reduce boilerplate code
#define CFG_GET(struct_name, root, field) struct_name.field = get_json_field(root, #field)

STATIC size_t read_file(char **buf, const char *filename);
STATIC char *get_json_field(yajl_val root, const char *field_name);
STATIC int get_json_array(yajl_val root, const char *array_name, char **array_to_fill, int max_entries);
STATIC void parse_config(const char *config_file);

struct mpd_info *mpd;
struct config_options cfg;
pthread_t main_thread_id;
char *program_name_arg;
char *config_file_arg;

int initialize(int argc, char *argv[], int *fd_args) {

	int opt, operation = 0;
	char *config = NULL;

	while ((opt = getopt(argc, argv, "udf:")) != -1) {
		switch (opt) {
			case 'u':
				operation = 1;
				break;
			case 'd':
				operation = -1;
				break;
			case 'f':
				fd_args[IRC]         = optarg[IRC];
				fd_args[MURM_LISTEN] = optarg[MURM_LISTEN];
				fd_args[MURM_ACCEPT] = optarg[MURM_ACCEPT];
				break;
			default:
				exit_msg("Usage: %s [<-u | -d> <-f fd>] [config_file]", argv[0]);
		}
	}
	if (optind < argc)
		config = config_file_arg = argv[optind];

	program_name_arg = argv[0];
	main_thread_id = pthread_self();
	srandom(time(NULL));
	signal(SIGPIPE, SIG_IGN); // Don't exit program when writing to a closed socket
	setlinebuf(stdout); // Flush on each line
	parse_config(config ? config : DEFAULT_CONFIG_NAME);

	sqlite3_initialize();
	if (!setup_database())
		exit_msg("Could not setup database");

	// Initialize curl library and setup mutexes and callbacks required by openssl for https access
	curl_global_init(CURL_GLOBAL_ALL);
	if (!openssl_crypto_init())
		exit_msg("Could not initialize openssl locks");

	if (*cfg.oauth_consumer_key && *cfg.oauth_consumer_secret && *cfg.oauth_token && *cfg.oauth_token_secret)
		cfg.twitter_details_set = true;

	if (*cfg.wolframalpha_api_key)
		setenv("WOLFRAMALPHA_API_KEY", cfg.wolframalpha_api_key, true);

	return operation;
}

STATIC size_t read_file(char **buf, const char *filename) {

	FILE *file;
	struct stat st;
	size_t n = 0;

	file = fopen(filename, "r");
	if (!file) {
		fprintf(stderr, "fopen error: ");
		return 0;
	}
	if (fstat(fileno(file), &st)) {
		fprintf(stderr, "fstat fail: ");
		goto cleanup;
	}
	if (!st.st_size || st.st_size > CONFSIZE) {
		fprintf(stderr, "File too small/big: ");
		goto cleanup;
	}
	*buf = malloc_w(st.st_size + 1);
	n = fread(*buf, sizeof(char), st.st_size, file);
	if (n != (unsigned) st.st_size) {
		fprintf(stderr, "fread error: ");
		fclose(file);
		return 0;
	}
	(*buf)[st.st_size] = '\0';

cleanup:
	fclose(file);
	return n;
}

STATIC char *get_json_field(yajl_val root, const char *field_name) {

	yajl_val val = yajl_tree_get(root, CFG(field_name), yajl_t_string);
	if (!val)
		exit_msg("%s: missing / wrong type", field_name);

	return YAJL_GET_STRING(val);
}

STATIC bool get_json_bool(yajl_val root, const char *field_name) {

	// Only accept true or false value
	yajl_val val = yajl_tree_get(root, CFG(field_name), yajl_t_any);
	if (!val)
		exit_msg("%s: missing", field_name);

	if (val->type != yajl_t_true && val->type != yajl_t_false)
		exit_msg("%s: wrong type", field_name);

	return YAJL_IS_TRUE(val);
}

STATIC int get_json_array(yajl_val root, const char *array_name, char **array_to_fill, int max_entries) {

	int array_size;
	yajl_val val, array;

	array = yajl_tree_get(root, CFG(array_name), yajl_t_array);
	if (!array)
		exit_msg("%s: missing / wrong type", array_name);

	array_size = YAJL_GET_ARRAY(array)->len;
	if (array_size > max_entries) {
		array_size = max_entries;
		fprintf(stderr, "%s limit (%d) reached. Ignoring rest\n", array_name,  max_entries);
	}
	for (int i = 0; i < array_size; i++) {
		val = YAJL_GET_ARRAY(array)->values[i];
		array_to_fill[i] = YAJL_GET_STRING(val);
	}
	return array_size;
}

STATIC char *expand_path(char *path) {

	char *expanded_path, *HOME;

	// Expand tilde '~' by reading the HOME environment variable
	HOME = getenv("HOME");

	if (*path != '~')
		return path;

	expanded_path = malloc_w(PATHLEN);
	snprintf(expanded_path, PATHLEN, "%s%s", HOME, path + 1);

	return expanded_path;
}

STATIC void parse_config(const char *config_file) {

	yajl_val root;
	char errbuf[1024], *buf = NULL;

	if (!read_file(&buf, config_file))
		exit_msg("%s", config_file);

	root = yajl_tree_parse(buf, errbuf, sizeof(errbuf));
	if (!root)
		exit_msg("%s", errbuf);

	// Free original buffer since we have a duplicate in root now
	free(buf);

	CFG_GET(cfg, root, server);
	CFG_GET(cfg, root, port);
	CFG_GET(cfg, root, nick);
	CFG_GET(cfg, root, user);
	CFG_GET(cfg, root, nick_password);
	CFG_GET(cfg, root, bot_version);
	CFG_GET(cfg, root, quit_message);
	CFG_GET(cfg, root, github_repo);
	CFG_GET(cfg, root, murmur_port);
	CFG_GET(cfg, root, mpd_port);
	CFG_GET(cfg, root, mpd_database);
	CFG_GET(cfg, root, mpd_random_state);
	CFG_GET(cfg, root, fifo_name);
	CFG_GET(cfg, root, db_name);
	CFG_GET(cfg, root, oauth_consumer_key);
	CFG_GET(cfg, root, oauth_consumer_secret);
	CFG_GET(cfg, root, oauth_token);
	CFG_GET(cfg, root, oauth_token_secret);
	CFG_GET(cfg, root, google_shortener_api_key);
	CFG_GET(cfg, root, wolframalpha_api_key);

	cfg.mpd_database      = expand_path(cfg.mpd_database);
	cfg.mpd_random_state  = expand_path(cfg.mpd_random_state);
	cfg.fifo_name         = expand_path(cfg.fifo_name);
	cfg.db_name           = expand_path(cfg.db_name);
	cfg.channels_set      = get_json_array(root, "channels",    cfg.channels,    MAXCHANS);
	cfg.access_list_count = get_json_array(root, "access_list", cfg.access_list, MAXACCLIST);
	cfg.verbose           = get_json_bool(root, "verbose");
}

int setup_irc(Irc *server, int *fd_args) {

	Mqueue mq;
	int ircfd;
	pthread_t tid;
	pthread_attr_t attr;

	*server = irc_connect(cfg.server, cfg.port, fd_args[IRC]);
	if (!*server)
		exit_msg("Irc connection failed");

	ircfd = get_socket(*server);
	mq = mqueue_init(ircfd);
	if (!mq)
		exit_msg("message queue initialization failed");

	set_mqueue(*server, mq);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&tid, &attr, mqueue_start, mq))
		exit_msg("Could not start queue");

	pthread_attr_destroy(&attr);
	set_nick(*server, cfg.nick);
	set_user(*server, cfg.user);
	for (int i = 0; i < cfg.channels_set; i++)
		join_channel(*server, cfg.channels[i]);

	return ircfd;
}

void setup_mumble(struct pollfd *pfd, int *fd_args) {

	if (fd_args[MURM_LISTEN] <= 0) {
		if (add_murmur_callbacks(cfg.murmur_port))
			pfd[MURM_LISTEN].fd = sock_listen(LOCALHOST, CB_LISTEN_PORT_S);
		else
			fprintf(stderr, "Could not connect to Murmur\n");
	} else {
		pfd[MURM_LISTEN].fd = fd_args[MURM_LISTEN];
		if (fd_args[MURM_ACCEPT] > 0) {
			pfd[MURM_ACCEPT].fd = fd_args[MURM_ACCEPT];
			pfd[MURM_LISTEN].events = 0;
		}
	}
}

int setup_mpd(void) {

	mpd = calloc_w(sizeof(*mpd));
	mpd->announce = OFF;
	if (!access(cfg.mpd_random_state, F_OK))
		mpd->random = ON;

	mpd->fd = mpd_connect(cfg.mpd_port);
	if (mpd->fd < 0)
		fprintf(stderr, "Could not connect to MPD\n");

	return mpd->fd;
}

int setup_fifo(FILE **stream) {

	mode_t old;
	struct stat st;
	int fifo, dummy, tries_left = 2;

	// Ensure we get the permissions we asked
	old = umask(0);
	do {
		fifo = open(cfg.fifo_name, O_RDONLY | O_NONBLOCK);
		if (!fstat(fifo, &st))
			if (S_ISFIFO(st.st_mode) && (FIFO_PERMISSIONS == (st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO))))
				break;

		close(fifo);
		if (remove(cfg.fifo_name))
			perror("remove");

		if (mkfifo(cfg.fifo_name, FIFO_PERMISSIONS))
			goto cleanup;

	} while (tries_left--);

	umask(old); // Restore mask
	if (tries_left == -1)
		goto cleanup;

	// Ensure we never get EOF
	dummy = open(cfg.fifo_name, O_WRONLY);
	if (dummy == -1) {
		close(fifo);
		goto cleanup;
	}
	*stream = fdopen(fifo, "r");
	if (!*stream) {
		close(fifo);
		close(dummy);
		goto cleanup;
	}
	return fifo; // Success

cleanup:
	perror("Could not setup FIFO");
	return -1;
}

void cleanup(void) {

	free(mpd);
	openssl_crypto_cleanup();
	curl_global_cleanup();
	close_database();
	sqlite3_shutdown();
}
