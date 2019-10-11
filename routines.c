/*
 * Simple co-routine library for POSIX C
 *
 * Author:  Curtis Millar
 * Date:    11 October 2019
 * Licence: MIT
 */

#include <assert.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>

#include <routines.h>

#define STACK_SIZE (4096 * 8)

/* A message from a routine queue */
typedef struct message {
	/* Message to be sent */
	void *message;
	/* Routine blocked on send */
	routines_coroutine_t *sender;
	/* Queue to use for reply */
	routines_queue_t *reply_queue;
	/* Next message in queue */
	struct message *next;
} message_t;

/* A queue of co-routines */
typedef struct {
	routines_coroutine_t *head;
	routines_coroutine_t *tail;
} coroutine_queue_t;

/* Concrete implementation of a routine queue */
struct routines_queue {
	/* Messages / routines waiting to be received */
	message_t *head;
	message_t **tail;
	/* Co-routines waiting to receive on the message queue */
	coroutine_queue_t recv_queue;
};

/* Concrete implementation of a co-routine */
struct routines_coroutine {
	/* Entrypoint function for co-routine */
	routines_task_t entrypoint;
	/* Arguments to pass when starting the co-routine */
	void *arg;
	/* Stack address of co-routine */
	unsigned char *stack_base;

	/* Suspended routine context */
	jmp_buf context;
	/* Current state of the c-oroutine */
	routines_state_t state;
	/* Co-routines waiting on this routine */
	coroutine_queue_t join_queue;

	/* Message queue entry where blocked */
	routines_coroutine_t **message;
	/* Receive queue qhere blocked */
	coroutine_queue_t *queue;

	/* Previous routine in ready / block queue */
	routines_coroutine_t *prev;
	/* Next routine in ready / block queue */
	routines_coroutine_t *next;
};

/* Co-routine stack list */
typedef struct stack {
	unsigned char *stack_base;
	struct stack *next;
} stack_t;

/* Global state */

/* The initial task context */
static struct {
	jmp_buf context;
} root_task;

/* Currently executing co-routine */
static routines_coroutine_t *current_coroutine;

/* Co-routine that just exited */
static routines_coroutine_t *exited_coroutine = NULL;

/* Queue of ready coroutines */
static coroutine_queue_t ready_queue;

/* Unused stacks */
static stack_t *unused_stacks;

/*
 * Message queue managment
 */

/* Add a message to the queue. */
static void enqueue_message(
	routines_queue_t *queue,
	void *message,
	routines_coroutine_t *sender,
	routines_queue_t *reply_queue
);

/*
 * Remove a message from the queue
 *
 * Places sender on reply queue if provided.
 */
static void *dequeue_message(
	routines_queue_t *queue,
	routines_queue_t **reply_queue
);

/* Check if there are any pending messages */
static bool pending_messages(routines_queue_t *queue);

/* Coroutine queue management */

/* Enqueue a co-routine */
static void coroutine_enqueue(
	coroutine_queue_t *queue,
	routines_coroutine_t *coroutine
);

/* Dequeue a co-routine */
static routines_coroutine_t *coroutine_dequeue(
	coroutine_queue_t *queue
);

/* Remove a co-routine from its queue */
static void coroutine_remove(routines_coroutine_t *coroutine);

/*
 * Stack allocation
 */
static unsigned char *alloc_stack(void);
static void free_stack(unsigned char *stack_base);
static void push_stack(unsigned char *stack_base);
static unsigned char *pop_stack(void);

/*
 * Communication primitives
 */

/* Primitive send operation */
static void send(
	routines_queue_t *send_queue,
	void *message,
	routines_coroutine_t *sender,
	routines_queue_t *reply_queue
);

/* Primitive recv operation */
static void *recv(
	routines_queue_t *recv_queue,
	routines_queue_t **reply_queue
);

/*
 * Co-routine management
 */

/*
 * Transfer execution to another coroutine
 *
 * If coroutine is NULL, to the next ready co-routine or to the initial
 * process thread if none is available. The current coroutine is enqueud
 * in the passed queue, if any.
 */
static void transfer(
	coroutine_queue_t *queue,
	routines_state_t state,
	routines_coroutine_t *coroutine
);

/* Call a function on a new stack. */
static inline void call_on_stack(
	void (*callback)(routines_coroutine_t *),
	routines_coroutine_t *coroutine
);

/* Entryoupint for a new co-routine */
static void routine_entry(routines_coroutine_t *coroutine);

/*
 * External interface
 */

routines_coroutine_t *routines_spawn(routines_task_t task, void *arg) {
	assert(task != NULL);

	routines_coroutine_t *coroutine = malloc(sizeof(*coroutine));
	*coroutine = (routines_coroutine_t) {
		.entrypoint = task,
		.arg = arg,
		.stack_base = alloc_stack(),
	};

	routines_coroutine_t *self = current_coroutine;

	int jmp_result;
	if (self != NULL) {
		coroutine_enqueue(&ready_queue, self);
		jmp_result = setjmp(self->context);
	} else {
		jmp_result = setjmp(root_task.context);
	}

    if (!jmp_result) {
		call_on_stack(routine_entry, coroutine);
	}

	if (exited_coroutine != NULL) {
		free_stack(exited_coroutine->stack_base);
		exited_coroutine->stack_base = NULL;
		exited_coroutine = NULL;
	}

	return coroutine;
}

void routines_destroy(routines_coroutine_t *coroutine) {
	assert(coroutine != NULL);

	routines_suspend(coroutine);
	coroutine_queue_t *join_queue = &coroutine->join_queue;
	routines_coroutine_t *joined = coroutine_dequeue(join_queue);
	while (joined != NULL) {
		routines_resume(joined);
		joined = coroutine_dequeue(join_queue);
	}

	if (coroutine->stack_base != NULL) {
		free_stack(coroutine->stack_base);
	}

	free(coroutine);
}

routines_coroutine_t *routines_self(void) {
	return current_coroutine;
}

routines_state_t routines_state(routines_coroutine_t *coroutine) {
	assert(coroutine != NULL);

	return coroutine->state;
}

void routines_yield(void) {
	transfer(&ready_queue, ROUTINES_RUNNING, NULL);
}

void routines_join(routines_coroutine_t *coroutine) {
	assert(current_coroutine != NULL);
	assert(coroutine != NULL);

	transfer(&coroutine->join_queue, ROUTINES_BLOCKED_JOIN, NULL);
}

void routines_suspend(routines_coroutine_t *coroutine) {
	assert(coroutine != NULL);

	if (coroutine->message != NULL) {
		/* Remove from send queue, leaving message */
		*coroutine->message = NULL;
		coroutine->message = NULL;
	}

	if (coroutine->queue != NULL) {
		/* remove from any other queues */
		coroutine_remove(coroutine);
	}

	coroutine->state = ROUTINES_SUSPENDED;

	if (coroutine == current_coroutine) {
		transfer(NULL, ROUTINES_SUSPENDED, NULL);
	}
}

void routines_resume(routines_coroutine_t *coroutine) {
	assert(coroutine != NULL);
	assert(coroutine != current_coroutine);
	assert(coroutine->state != ROUTINES_COMPLETED);

	/* Suspend to remove from ay queues */
	routines_suspend(coroutine);

	coroutine->state = ROUTINES_RUNNING;
	coroutine_enqueue(&ready_queue, coroutine);
}

routines_queue_t *routines_queue_create(void) {
	routines_queue_t *queue = malloc(sizeof(routines_queue_t));
	*queue = (routines_queue_t) {
		.head = NULL,
		.tail = &queue->head,
		.recv_queue = (coroutine_queue_t) {
			.head = NULL,
			.tail = NULL,
		},
	};
	return queue;
}

void routines_queue_destroy(routines_queue_t *queue) {
	assert(queue != NULL);

	while (pending_messages(queue)) {
		dequeue_message(queue, NULL);
	}

	routines_coroutine_t *server
		= coroutine_dequeue(&queue->recv_queue);
	while (server != NULL) {
		routines_resume(server);
		server = coroutine_dequeue(&queue->recv_queue);
	}

	free(queue);
}

void routines_send(routines_queue_t *queue, void *message) {
	assert(current_coroutine != NULL);
	assert(queue != NULL);

	send(queue, message, current_coroutine, NULL);
}

void *routines_wait(routines_queue_t *queue) {
	assert(current_coroutine != NULL);
	assert(queue != NULL);

	return recv(queue, NULL);
}

void routines_signal(routines_queue_t *queue, void *message) {
	assert(current_coroutine != NULL);
	assert(queue != NULL);

	send(queue, message, NULL, NULL);
}

void *routines_read(routines_queue_t *queue) {
	assert(current_coroutine != NULL);
	assert(queue != NULL);

	if (pending_messages(queue)) {
		return recv(queue, NULL);
	} else {
		return NULL;
	}
}

void *routines_call(
	routines_queue_t *send_queue,
	void *message,
	routines_queue_t *reply_queue
) {
	assert(current_coroutine != NULL);
	assert(send_queue != NULL);
	assert(reply_queue != NULL);

	send(send_queue, message, NULL, reply_queue);
	return recv(reply_queue, NULL);
}

void *routines_recv(
	routines_queue_t *recv_queue,
	routines_queue_t **reply_queue
) {
	assert(current_coroutine != NULL);
	assert(recv_queue != NULL);

	return recv(recv_queue, reply_queue);
}

void routines_post(
	routines_queue_t *send_queue,
	void *message,
	routines_queue_t *reply_queue
) {
	assert(current_coroutine != NULL);
	assert(send_queue != NULL);

	send(send_queue, message, NULL, reply_queue);
}

/*
 * Internal Implementations
 */

static void enqueue_message(
	routines_queue_t *queue,
	void *message,
	routines_coroutine_t *sender,
	routines_queue_t *reply_queue
) {
	assert(queue != NULL);

	message_t *new_tail = malloc(sizeof(message_t));
	*new_tail = (message_t) {
		.message = message,
		.sender = sender,
		.reply_queue = reply_queue,
		.next = NULL,
	};

	*queue->tail = new_tail;
	queue->tail = &new_tail->next;

	if (sender != NULL) {
		sender->message = &new_tail->sender;
		transfer(NULL, ROUTINES_BLOCKED_SEND, NULL);
	}
}

static void *dequeue_message(
	routines_queue_t *queue,
	routines_queue_t **reply_queue
) {
	assert(queue != NULL);

	void *message = NULL;
	message_t *head = queue->head;
	if (head != NULL) {
		message = head->message;
		if (head->sender != NULL) {
			routines_resume(head->sender);
		}
		if (reply_queue != NULL) {
			*reply_queue = head->reply_queue;
		}
		queue->head = head->next;
		free(head);
	}

	if (queue->head == NULL) {
		queue->tail = &queue->head;
	}

	return message;
}

static bool pending_messages(routines_queue_t *queue) {
	return queue != NULL && queue->head != NULL;
}

static void coroutine_enqueue(
	coroutine_queue_t *queue,
	routines_coroutine_t *coroutine
) {
	assert(queue != NULL);
	assert(coroutine != NULL);
	assert(coroutine->next == NULL);
	assert(coroutine->prev == NULL);

	if (queue->tail != NULL) {
		coroutine->prev = queue->tail;
		queue->tail->next = coroutine;
	} else {
		queue->head = coroutine;
	}
	queue->tail = coroutine;
	coroutine->queue = queue;
}

static routines_coroutine_t *coroutine_dequeue(
	coroutine_queue_t *queue
) {
	assert(queue != NULL);
	routines_coroutine_t *head = queue->head;

	if (head != NULL) {
		queue->head = head->next;
		head->queue = NULL;
	}

	if (queue->head == NULL) {
		queue->tail = NULL;
	}

	return head;
}

static void coroutine_remove(routines_coroutine_t *coroutine) {
	assert(coroutine != NULL);
	assert(coroutine->queue != NULL);

	coroutine_queue_t *queue = coroutine->queue;

	if (coroutine->prev != NULL) {
		coroutine->prev->next = coroutine->next;
	} else {
		assert(queue->head == coroutine);
		queue->head = coroutine->next;
	}

	if (coroutine->next != NULL) {
		coroutine->next->prev = coroutine->prev;
	} else {
		assert(queue->tail == coroutine);
		queue->tail = coroutine->prev;
	}

	coroutine->next = NULL;
	coroutine->prev = NULL;
	coroutine->queue = NULL;
}

static unsigned char *alloc_stack(void) {
	unsigned char *stack = pop_stack();

	if (stack == NULL) {
		stack = malloc(STACK_SIZE);
		stack += STACK_SIZE;
	}
	return stack;
}

static void free_stack(unsigned char *stack_base) {
	push_stack(stack_base);
}

static void push_stack(unsigned char *stack_base) {
	stack_t *stack = malloc(sizeof(stack_t));
	*stack = (stack_t) {
		.stack_base = stack_base,
		.next = unused_stacks,
	};
	unused_stacks = stack;
}

static unsigned char *pop_stack(void) {
	unsigned char *stack_base = NULL;
	stack_t *stack = unused_stacks;

	if (stack != NULL) {
		stack_base = stack->stack_base;
		unused_stacks = stack->next;
		free(stack);
	}

	return stack_base;
}

static void send(
	routines_queue_t *send_queue,
	void *message,
	routines_coroutine_t *sender,
	routines_queue_t *reply_queue
) {
	assert(send_queue != NULL);

	routines_coroutine_t *server =
		coroutine_dequeue(&send_queue->recv_queue);

	if (server != NULL) {
		enqueue_message(send_queue, message, NULL, reply_queue);
		transfer(&ready_queue, ROUTINES_RUNNING, server);
	} else {
		enqueue_message(send_queue, message, sender, reply_queue);
	}
}

static void *recv(
	routines_queue_t *recv_queue,
	routines_queue_t **reply_queue
) {
	assert(recv_queue != NULL);

	if (!pending_messages(recv_queue)) {
		transfer(
			&recv_queue->recv_queue,
			ROUTINES_BLOCKED_RECV,
			NULL
		);
	}

	return dequeue_message(recv_queue, reply_queue);
}

static void transfer(
	coroutine_queue_t *queue,
	routines_state_t state,
	routines_coroutine_t *coroutine
) {
	routines_coroutine_t *self = current_coroutine;

	if (self != NULL) {
		self->state = state;
		if (queue != NULL) {
			coroutine_enqueue(queue, self);
		}
	}

	if (coroutine == NULL) {
		coroutine = coroutine_dequeue(&ready_queue);
	}

	current_coroutine = coroutine;

	int jmp_result;
	if (self != NULL) {
		jmp_result = setjmp(self->context);
	} else {
		jmp_result = setjmp(root_task.context);
	}

    if (!jmp_result) {
		if (current_coroutine != NULL) {
			current_coroutine->state = ROUTINES_RUNNING;
        	longjmp(current_coroutine->context, 1);
		} else {
        	longjmp(root_task.context, 1);
		}
    }

	if (exited_coroutine != NULL) {
		free_stack(exited_coroutine->stack_base);
		exited_coroutine->stack_base = NULL;
		exited_coroutine = NULL;
	}
}

static void routine_entry(routines_coroutine_t *coroutine) {
	if (current_coroutine != NULL) {
		coroutine_enqueue(&ready_queue, current_coroutine);
	}

	current_coroutine = coroutine;
	coroutine->entrypoint(coroutine->arg);

	coroutine_queue_t *join_queue = &coroutine->join_queue;
	routines_coroutine_t *joined = coroutine_dequeue(join_queue);
	while (joined != NULL) {
		routines_resume(joined);
		joined = coroutine_dequeue(join_queue);
	}

	exited_coroutine = coroutine;
	coroutine->state = ROUTINES_COMPLETED;

	current_coroutine = coroutine_dequeue(&ready_queue);
	if (current_coroutine != NULL) {
		longjmp(current_coroutine->context, 1);
	} else {
		longjmp(root_task.context, 1);
	}
}

#if !defined(__GNUC__) && !defined(__clang__)
#error "call_on_stack not implemented for this compiler"
#endif

static inline void call_on_stack(
	void (*callback)(routines_coroutine_t *),
	routines_coroutine_t *coroutine
) {
#if defined(__x86_64)
    asm volatile (
        "movq %[stack], %%rsp\n"
        "movq %[args], %%rdi\n"
        "push %%rbp\n"
        "push %%rsp\n"
        "callq *%[cb]\n"
        :
        : [stack] "r" (coroutine->stack_base)
		, [args] "r" (coroutine)
		, [cb] "r" (callback)
        : "rdi", "memory"
    );
#elif defined(__arm__)
#error "call_on_stack not implemented arm"
#elif defined(__aarch64__)
#error "call_on_stack not implemented aarch64"
#elif defined(__i386)
#error "call_on_stack not implemented IA-32"
#elif defined(__riscv)
#error "call_on_stack not implemented RISC-V"
#else
#error "call_on_stack not implemented for target architecture"
#endif
}
