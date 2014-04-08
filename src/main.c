#include <stdio.h>
#include <poll.h>
#include <pthread.h>
#include "socket.h"
#include "irc.h"
#include "murmur.h"
#include "mpd.h"
#include "common.h"
#include "ratelimit.h"

#define TIMEOUT 300 * 1000 // Timeout in milliseconds for the poll function

extern struct mpd_info *mpd;
enum { IRC, MURM_LISTEN, MURM_ACCEPT, MPD, PFDS };

int main(int argc, char *argv[]) {

	Irc irc_server;
	struct rate_limit rtl;
	pthread_t tid;
	int i, ready, murm_listenfd = -1;
	struct pollfd pfd[PFDS] = {
		{ .fd = -1, .events = POLLIN },
		{ .fd = -1, .events = POLLIN },
		{ .fd = -1, .events = POLLIN },
		{ .fd = -1, .events = POLLIN }
	};

	initialize(argc, argv);
	irc_server = irc_connect(cfg.server, cfg.port);
	if (!irc_server)
		exit_msg("Irc connection failed\n");

	rtl = ratelimit_init();
	rtl.ircfd = pfd[IRC].fd = get_socket(irc_server);
	pthread_create(&tid, NULL, ratelimit_loop, &rtl);

	set_nick(irc_server, cfg.nick);
	set_user(irc_server, cfg.user);
	for (i = 0; i < cfg.channels_set; i++)
		join_channel(irc_server, cfg.channels[i]);

	if (add_murmur_callbacks(cfg.murmur_port))
		murm_listenfd = pfd[MURM_LISTEN].fd = sock_listen(LOCALHOST, CB_LISTEN_PORT_S);
	else
		fprintf(stderr, "Could not connect to Murmur\n");

	mpd->fd = pfd[MPD].fd = mpd_connect(cfg.mpd_port);
	if (mpd->fd < 0)
		fprintf(stderr, "Could not connect to MPD\n");

	while ((ready = poll(pfd, PFDS, TIMEOUT)) > 0) {
		// Keep reading & parsing lines as long the connection is active and act on any registered actions found
		if (pfd[IRC].revents & POLLIN)
			while (parse_irc_line(irc_server) > 0);

		if (pfd[MURM_LISTEN].revents & POLLIN) {
			pfd[MURM_ACCEPT].fd = accept_murmur_connection(murm_listenfd);
			if (pfd[MURM_ACCEPT].fd > 0)
				pfd[MURM_LISTEN].fd = -1; // Stop listening for connections
		}

		if (pfd[MURM_ACCEPT].revents & POLLIN) {
			if (!listen_murmur_callbacks(irc_server, pfd[MURM_ACCEPT].fd)) {
				pfd[MURM_ACCEPT].fd = -1;
				pfd[MURM_LISTEN].fd = murm_listenfd; // Start listening again for Murmur connections
			}
		}
		if (pfd[MPD].revents & POLLIN)
			if (!print_song(irc_server, default_channel(irc_server)))
				pfd[MPD].fd = mpd->fd = mpd_connect(cfg.mpd_port);
	}
	// If we reach here, it means we got disconnected from server. Exit with error (1)
	if (ready == -1)
		perror("poll");
	else
		fprintf(stderr, "%d minutes passed without getting a message, exiting...\n", TIMEOUT / 1000 / 60);

	quit_server(irc_server, cfg.quit_message);
	ratelimit_destroy(rtl);
	cleanup();
	return 1;
}
