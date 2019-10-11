/*
 * Ping-pong example with coroutines
 *
 * Author:  Curtis Millar
 * Date:    11 October 2019
 * Licence: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <routines.h>

#define NUM_CLIENTS 2
#define NUM_PINGS   5

typedef struct {
	routines_coroutine_t *coroutine;
	routines_queue_t *message_queue;
} server_t;

typedef struct {
	routines_coroutine_t *coroutine;
	routines_queue_t *message_queue;
	int id;
	int pings;
	int pongs;
} client_t;

void server_task(void *arg) {
	server_t *server = arg;

	while (true) {
		printf("[SERVER] Waiting for message\n");
		routines_queue_t *reply_queue;
		client_t *client = routines_recv(
			server->message_queue,
			&reply_queue
		);
		client->pongs += 1;
		printf(
			"[SERVER] Pong #%d for client #%d\n",
			client->pongs,
			client->id
		);
		routines_signal(reply_queue, client);
	}
}

void client_task(void *arg) {
	client_t *client = arg;
	routines_queue_t *reply_queue = routines_queue_create();

	for (int i; i < NUM_PINGS; i += 1) {
		client->pings +=1;
		printf(
			"[CLIENT #%d] Ping #%d\n",
			client->id,
			client->pings
		);
		client_t *response = routines_call(
			client->message_queue,
			client,
			reply_queue
		);
		printf(
			"[CLIENT #%d] Pong #%d from server for client #%d\n",
			client->id,
			response->pongs,
			response->id
		);
	}

	routines_queue_destroy(reply_queue);
}

int main(void) {
	server_t server = {};
	server.message_queue = routines_queue_create();

	/* Start each client */
	client_t clients[NUM_CLIENTS] = {};
	for (size_t c = 0; c < NUM_CLIENTS; c += 1) {
		printf("[ROOT] Starting client %lu\n", c);
		client_t *client = &clients[c];
		*client = (client_t) {
			.message_queue = server.message_queue,
			.id = c,
			.pings = 0,
			.pongs = 0,
		};
		client->coroutine = routines_spawn(client_task, client);
	}

	/* Start the server listening on the message queue */
	printf("[ROOT] Starting server\n");
	server.coroutine = routines_spawn(server_task, &server);

	printf("[ROOT] All tasks completed!\n");

	routines_destroy(server.coroutine);
	for (size_t c = 0; c < NUM_CLIENTS; c += 1) {
		routines_destroy(clients[c].coroutine);
	}
	routines_queue_destroy(server.message_queue);
}
