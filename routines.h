/*
 * Simple co-routine library for POSIX C
 *
 * Author:  Curtis Millar
 * Date:    11 October 2019
 * Licence: MIT
 */

typedef enum {
	ROUTINES_COMPLETED,
	ROUTINES_SUSPENDED,
	ROUTINES_RUNNING,
	ROUTINES_BLOCKED_SEND,
	ROUTINES_BLOCKED_RECV,
	ROUTINES_BLOCKED_JOIN,
} routines_state_t;

/* A function that defines the work of a specific task */
typedef void (*routines_task_t)(void *);

/* A co-routine */
typedef struct routines_coroutine routines_coroutine_t;

/* A message passing queue */
typedef struct routines_queue routines_queue_t;

/* Spawn a new co-routine as a separate task */
routines_coroutine_t *routines_spawn(routines_task_t task, void *arg);

/*
 * Destroy a routine
 *
 * The routine is suspended as though `routines_suspend` was called
 * before the routine is destroyed.
 */
void routines_destroy(routines_coroutine_t *coroutine);

/* Get the currently running routine */
routines_coroutine_t *routines_self(void);

/* Get the state of a co-routine */
routines_state_t routines_state(routines_coroutine_t *coroutine);

/*
 * Yield time to another co-routine
 *
 * Returns to root initial thread if no co-routines are available.
 */
void routines_yield(void);

/* Wait for a co-routine to complete */
void routines_join(routines_coroutine_t *coroutine);

/*
 * Suspend a running co-routine
 *
 * If the co-routine is blocked on a message queue it is removed from
 * the queue. If it was receiving then when it is resumed it will
 * receive a NULL message and a NULL reply queue.
 */
void routines_suspend(routines_coroutine_t *coroutine);

/* Suspend the currently executing co-routine */
static inline void routines_suspend_self(void) {
	routines_suspend(routines_self());
}

/*
 * Resume a suspended co-routine
 *
 * If the co-routine is blocked on a message queue it is removed from
 * the queue. If it was receiving then it will receive a NULL message
 * and a NULL reply queue.
 */
void routines_resume(routines_coroutine_t *coroutine);

/*
 * Synchronisation and communication primitives
 */

/* Create a new messaging queue */
routines_queue_t *routines_queue_create(void);

/*
 * Destroy a messaging queue
 *
 * All messages in the queue are lost and all co-routines blocked on the
 * queue are resumed.
 */
void routines_queue_destroy(routines_queue_t *queue);

/* Send a message to a queue, blocking until the message is received */
void routines_send(routines_queue_t *queue, void *message);

/*
 * Receive a message from a queue, blocking until a message is
 * available
 */
void *routines_wait(routines_queue_t *queue);

/* Send a message to a queue without blocking */
void routines_signal(routines_queue_t *queue, void *message);

/*
 * Receive a message from a queue without blocking
 *
 * Returns NULL if no message is available.
 */
void *routines_read(routines_queue_t *queue);

/* Send a message to another queue and wait for a reply */
void *routines_call(
	routines_queue_t *send_queue,
	void *message,
	routines_queue_t *reply_queue
);

/*
 * Receive a message from a message queue along with a message queue
 * on which the caller is waiting for a reply
 */
void *routines_recv(
	routines_queue_t *recv_queue,
	routines_queue_t **reply_queue
);

/*
 * Send a message to another queue without blocking, providing another
 * queue for a later reply
 */
void routines_post(
	routines_queue_t *send_queue,
	void *message,
	routines_queue_t *reply_queue
);
