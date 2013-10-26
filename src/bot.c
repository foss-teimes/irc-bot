#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>
#include <yajl/yajl_tree.h>
#include "bot.h"
#include "irc.h"
#include "curl.h"
#include "common.h"


void help(Irc server, Parsed_data pdata) {

	send_message(server, pdata.target, "%s", "url, mumble, fail, github, ping, traceroute, dns, uptime, roll");
	send_message(server, pdata.target, "%s", "MPD: play, playlist, history, current, next, random, stop, seek, announce");
}

void bot_fail(Irc server, Parsed_data pdata) {

	int r, clr_r;
	size_t len, maxlen, sum = 0;
	char quote[QUOTELEN];

	if (!cfg.quote_count)
		return;

	// Make sure the seed is different even if we call the command twice in a second
	srand(time(NULL) + getpid());
	r      = rand() % cfg.quote_count;
	clr_r  = rand() % COLORCOUNT;
	maxlen = strlen(cfg.quotes[r]);

	// Pick a random entry from the read-only quotes array and print it.
	// We use indexes and lengths since we can't make changes to the array
	while (sum < maxlen) {
		len = strcspn(cfg.quotes[r] + sum, "\n");
		if (!len)
			return;

		snprintf(quote, QUOTELEN, COLOR "%d%.*s", clr_r, (int) len, cfg.quotes[r] + sum);
		send_message(server, pdata.target, "%s", quote);
		sum += ++len; // Skip newline
	}
}

void url(Irc server, Parsed_data pdata) {

	char **argv, *temp, *short_url, *url_title = NULL;
	int argc;

	argv = extract_params(pdata.message, &argc);
	if (argc != 1)
		goto cleanup;

	// Check first parameter for a URL that contains at least one dot.
	if (!strchr(argv[0], '.'))
		goto cleanup;

	// Map shared memory for inter-process communication
	short_url = mmap(NULL, ADDRLEN + 1, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (short_url == MAP_FAILED) {
		perror("mmap");
		goto cleanup;
	}

	switch (fork()) {
	case -1:
		perror("fork");
		break;
	case 0:
		temp = shorten_url(argv[0]);
		if (temp) {
			strncpy(short_url, temp, ADDRLEN);
			free(temp);
		} else // Put a null char in the first byte if shorten_url fails so we can test for it in send_message
			*short_url = '\0';

		munmap(short_url, ADDRLEN + 1); // Unmap pages from child process
		_exit(EXIT_SUCCESS);
	default:
		url_title = get_url_title(argv[0]);
		wait(NULL); // Wait for child results before continuing

		// Only print short_url / title if they are not empty
		send_message(server, pdata.target, "%s -- %s", (*short_url ? short_url : ""), (url_title ? url_title : ""));
	}
	munmap(short_url, ADDRLEN + 1); // Unmap pages from parent as well

cleanup:
	free(argv);
	free(url_title);
}

void mumble(Irc server, Parsed_data pdata) {

	char *user_list;

	user_list = fetch_murmur_users();
	if (user_list) {
		send_message(server, pdata.target, "%s", user_list);
		free(user_list);
	}
}

void github(Irc server, Parsed_data pdata) {

	Github *commits;
	yajl_val root = NULL;
	char **argv, *short_url, repo[REPOLEN + 1];
	int argc, i, commit_count = 1;

	argv = extract_params(pdata.message, &argc);
	if (argc != 1 && argc != 2) {
		free(argv);
		return;
	}
	// If user is not supplied, substitute with a default one
	if (!strchr(argv[0], '/'))
		snprintf(repo, REPOLEN, "%s/%s", cfg.github_repo, argv[0]);
	else {
		strncpy(repo, argv[0], REPOLEN);
		repo[REPOLEN] = '\0';
	}

	// Do not return more than MAXCOMMITS
	if (argc == 2)
		commit_count = get_int(argv[1], MAXCOMMITS);

	commits = fetch_github_commits(repo, &commit_count, root);
	if (!commit_count)
		goto cleanup;

	// Print each commit info with it's short url in a seperate colorized line
	for (i = 0; i < commit_count; i++) {
		short_url = shorten_url(commits[i].url);
		send_message(server, pdata.target,
					PURPLE "[%.7s]"
					RESET " %.120s"
					ORANGE " --%s"
					BLUE " - %s",
					commits[i].sha, commits[i].msg, commits[i].name, (short_url ? short_url : ""));
		free(short_url);
	}
cleanup:
	yajl_tree_free(root);
	free(commits);
	free(argv);
}

void ping(Irc server, Parsed_data pdata) {

	char **argv, *cmd, count_str[10];
	int argc, count = 3;

	argv = extract_params(pdata.message, &argc);
	if (argc != 1 && argc != 2)
		goto cleanup;

	// Find if the IP is an v4 or v6. Weak parsing
	if (strchr(argv[0], '.'))
		cmd = "ping";
	else if (strchr(argv[0], ':'))
		cmd = "ping6";
	else
		goto cleanup;

	if (argc == 2)
		count = get_int(argv[1], MAXPINGS);

	sprintf(count_str, "%d", count);
	print_cmd_output(server, pdata.target, (char *[]) { cmd, "-c", count_str, argv[0], NULL });

cleanup:
	free(argv);
}

void traceroute(Irc server, Parsed_data pdata) {

	char **argv, *cmd;
	int argc;

	argv = extract_params(pdata.message, &argc);
	if (argc != 1)
		goto cleanup;

	if (strchr(argv[0], '.'))
		cmd = "traceroute";
	else if (strchr(argv[0], ':'))
		cmd = "traceroute6";
	else
		goto cleanup;

	// Don't send the following msg if the request was initiated in private
	if (strchr(pdata.target, '#'))
		send_message(server, pdata.target, "Printing results privately to %s", pdata.sender);

	// Limit max hops to 18
	print_cmd_output(server, pdata.sender, (char *[]) { cmd, "-m 18", argv[0], NULL });

cleanup:
	free(argv);
}

void dns(Irc server, Parsed_data pdata) {

	char **argv;
	int argc;

	argv = extract_params(pdata.message, &argc);
	if (argc != 1)
		goto cleanup;

	if (!strchr(argv[0], '.'))
		goto cleanup;

	print_cmd_output(server, pdata.target, (char *[]) { "nslookup", argv[0], NULL });

cleanup:
	free(argv);
}

void uptime(Irc server, Parsed_data pdata) {

	print_cmd_output_unsafe(server, pdata.target, "uptime");
}

void roll(Irc server, Parsed_data pdata) {

	char **argv;
	int argc, r, roll = DEFAULT_ROLL;

	srand(time(NULL) + getpid());
	argv = extract_params(pdata.message, &argc);
	if (argc != 1) {
		free(argv);
		return;
	}

	roll = get_int(argv[0], MAXROLL);
	if (roll == 1)
		roll = DEFAULT_ROLL;

	r = rand() % roll + 1;
	send_message(server, pdata.target, "%s rolls %d", pdata.sender, r);

	free(argv);
}
