/*
 * Single-threaded TCP server with async I/O
 *
 * Author:  Curtis Millar
 * Date:    11 October 2019
 * Licence: MIT
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <routines.h>

#define NULL_FD (-1)

#define LISTEN_PORT    1234
#define LISTEN_BACKLOG 128

#define TRY(e) {if (e < 0) { perror(#e); exit(EXIT_FAILURE); }}

typedef struct server     server_t;
typedef struct connection connection_t;
typedef struct wait       wait_t;

/* Socket server */
struct server {
	/* Server is live */
	bool live;

	/* Socket for incoming connections */
	int listen_fd;

	/* Poll for file connections */
	int epoll_fd;

	/* Coroutine for connection handling */
	routines_coroutine_t *connection_listener;

	/* Exited connections */
	connection_t *exited;
};

/* Connection handler */
struct connection {
	/* Co-routine for handler */
	routines_coroutine_t *coroutine;

	/* File descriptor being handled */
	int fd;

	/* Server connection associated with */
	server_t *server;

	/* Next connection in exited queue */
	connection_t *next;
};

/* Connection waiting on file descriptor */
struct wait {
	/* Waker to signal when file descriptor ready */
	routines_queue_t *waker;
	/* File descriptor being waited on */
	int fd;
	/* Events triggered on file descriptor */
	uint32_t revents;
};

/* Start a new server */
static void server_start(server_t *server);

/* Stop the server running */
static void server_stop(server_t *server);

/* Poll for events on file descriptors */
static void server_poll(server_t *server);

/* Wait for registered epoll events */
static void server_poll_once(server_t *server);

/* Wait for events on a file descriptor */
static uint32_t server_wait(server_t *server, int fd, uint32_t events);

/* Connection request handler */
static void listen_for_connections(void *);

/* Connection handler */
static void handle_connection(void *);

/* Connection management */
static connection_t *new_connection(server_t *server, int fd);
static void destroy_connection(connection_t *connection);

/* Register the connection as exited */
static void connection_exit(connection_t *connection);

/* Manage exited connections */
static void exited_push(connection_t **stack, connection_t *connection);
static connection_t *exited_pop(connection_t **stack);
static void exited_drain(connection_t **stack);

/* I/O suspend */
int main(void) {
	server_t server = {};
	server_start(&server);
	server_poll(&server);
	server_stop(&server);
	return EXIT_SUCCESS;
}

static void server_start(server_t *server) {
	/* Mark the server as being live */
	server->live = true;

	/* Initialise epoll */
	server->epoll_fd = epoll_create(1);
	TRY(server->epoll_fd);

	/* Set up a listening socket connection */
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(LISTEN_PORT),
		.sin_addr = (struct in_addr) {INADDR_ANY},
	};
	server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	TRY(server->listen_fd);
	TRY(bind(
		server->listen_fd,
		(struct sockaddr *)(&addr),
		sizeof(struct sockaddr_in)
	));
	TRY(listen(server->listen_fd, LISTEN_BACKLOG));

	/* Server exited queue */
	server->exited = NULL;

	/* start listening co-routine */
	server->connection_listener = routines_spawn(
		listen_for_connections,
		server
	);
}

static void server_stop(server_t *server) {
	assert(routines_self() == NULL);

	close(server->listen_fd);
	close(server->epoll_fd);
	exited_drain(&server->exited);
	routines_destroy(server->connection_listener);
}

static void server_poll(server_t *server) {
	while (server->live) {
		server_poll_once(server);
		routines_yield();
		exited_drain(&server->exited);
	}
}

static void server_poll_once(server_t *server) {
	struct epoll_event events[32];
	int num_events = epoll_wait(server->epoll_fd, events, 32, -1);
	TRY(num_events);

	for (size_t e = 0; e < num_events; e += 1) {
		wait_t *wait = events[e].data.ptr;
		wait->revents = events[e].events;
		epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, wait->fd, NULL);
		routines_signal(wait->waker, NULL);
	}
}

static uint32_t server_wait(server_t *server, int fd, uint32_t events) {
	wait_t wait = {
		.waker = routines_queue_create(),
		.fd = fd,
		.revents = 0,
	};
	struct epoll_event event = (struct epoll_event) {
		.events = events,
		.data.ptr = &wait,
	};

	epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, fd, &event);

	routines_wait(wait.waker);

	routines_queue_destroy(wait.waker);

	return wait.revents;
}

static void listen_for_connections(void *arg) {
	server_t *server = arg;

	struct sockaddr_in peer_addr;
	socklen_t peer_addr_size = sizeof(socklen_t);

	server_wait(server, server->listen_fd, EPOLLIN);
	while (true) {
		int peer_fd = accept(
			server->listen_fd,
			(struct sockaddr *)&peer_addr,
			&peer_addr_size
		);
		TRY(peer_fd);

		printf("[CONN] New connection on #%d\n", peer_fd);
		new_connection(server, peer_fd);
		server_wait(server, server->listen_fd, EPOLLIN);
	}
}

static void handle_connection(void *arg) {
	connection_t *connection = arg;
	char buffer[4096] = "ECHO: ";

	printf("[CLIENT #%d] Listening\n", connection->fd);
	server_wait(connection->server, connection->fd, EPOLLIN);
	while (strcmp(buffer + 6, "exit\n") != 0) {
		ssize_t bytes = read(connection->fd, buffer + 6, 4089);
		TRY(bytes);
		buffer[6 + bytes] = 0;
		printf("[CLIENT #%d] Message: %s\n", connection->fd, buffer + 6);
		server_wait(connection->server, connection->fd, EPOLLOUT);
		write(connection->fd, buffer, strlen(buffer));
		server_wait(connection->server, connection->fd, EPOLLIN);
	}

	printf("[CLIENT #%d] Closing\n", connection->fd);
	close(connection->fd);

	connection_exit(connection);
}

static connection_t *new_connection(server_t *server, int fd) {
	connection_t *connection = malloc(sizeof(connection_t));
	*connection = (connection_t) {
		.coroutine = NULL,
		.fd = fd,
		.next = NULL,
		.server = server,
	};

	connection->coroutine
		= routines_spawn(handle_connection, connection);

	return connection;
}

static void destroy_connection(connection_t *connection) {
	routines_destroy(connection->coroutine);
	free(connection);
}

static void connection_exit(connection_t *connection) {
	exited_push(&connection->server->exited, connection);
}

static void exited_push(connection_t **stack, connection_t *connection) {
	connection->next = *stack;
	*stack = connection;
}

static connection_t *exited_pop(connection_t **stack) {
	connection_t *popped = *stack;
	if (popped != NULL) {
		*stack = popped->next;
	}
	return popped;
}

static void exited_drain(connection_t **stack) {
	connection_t *exited = exited_pop(stack);
	while (exited != NULL) {
		destroy_connection(exited);
		exited = exited_pop(stack);
	}
}
