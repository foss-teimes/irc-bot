#ifndef INIT_H
#define INIT_H

/**
 * @file init.h
 * Initialization & setup functions that are called only once
 */

#include <stdio.h>
#include <stdbool.h>
#include <poll.h>
#include "irc.h"

#define DEFAULT_CONFIG_NAME "config.json"
#define FIFO_PERMISSIONS (S_IRUSR | S_IWUSR | S_IWGRP | S_IWOTH)

#define CONFSIZE      4096
#define PATHLEN       120
#define MAXACCLIST    10
#define POLL_TIMEOUT (300 * MILLISECS)

enum fds_array {IRC, MURM_LISTEN, MURM_ACCEPT, MPD, FIFO, TOTAL};

struct config_options {
	char *server;
	char *port;
	char *nick;
	char *nick_password;
	char *user;
	char *channels[MAXCHANS];
	int channels_set;
	char *bot_version;
	char *github_repo;
	char *quit_message;
	char *murmur_port;
	char *mpd_port;
	char *mpd_database;
	char *mpd_random_state;
	char *fifo_name;
	char *db_name;
	char *oauth_consumer_key;
	char *oauth_consumer_secret;
	char *oauth_token;
	char *oauth_token_secret;
	char *google_shortener_api_key;
	char *wolframalpha_api_key;
	bool twitter_details_set;
	char *access_list[MAXACCLIST];
	int access_list_count;
	bool verbose;
};

extern struct config_options cfg; //!< global struct with config's values

/** Parse arguments, load config, install signal handlers etc
 *  @param argc, argv  main's parameters unaltered
 *  @param fd          the file descriptor passed or 0
 *  @returns           0 for normal operation, > 0 for upgrade and < 0 for downgrade
 */
int initialize(int argc, char *argv[], int *fd);

//@{
/** The returned file descriptors are always valid. exit() is called on failure */
int setup_irc(Irc *server, int *fd_args);
int setup_mpd(void);
int setup_fifo(FILE **stream);
//@}

/** Mumble setup is more special because we have to handle 2 probable file descriptors */
void setup_mumble(struct pollfd *pfd, int *fd_args);

/** Cleanup init's mess */
void cleanup(void);

#endif

