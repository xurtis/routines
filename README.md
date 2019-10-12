Routines POSIX C co-routine library
===================================

`routines` is a simple POSIX C co-routine library with protected stacks
and simple synchronization objects and operations.

This library is **not thread-safe**.

Building
--------

Building is as simple as `make` which produces a static and shared
library.

Basic use
---------

### Spawning co-routines

Creating a co-routine is as simple as running a function.

```c
void task(void *arg) {
    char *message = arg;
    printf("'%s' for coroutine!\n");
}

int main(void) {
    routines_coroutine_t *coroutine
        = routines_spawn(task, "Hello, world!");

    routines_destroy(coroutine);
}
```

Message passing
---------------

Synchronization is performed using the message passing primitives. A
message payload isn't required when using the primitives, but it allows
for communication at the same time as synchronisation.

### Synchronous send

In a synchronous send, the sending co-routine will wait until a
receiving co-routine has received a message. This can act as a
rendezvous mechanism.

```c
routines_queue_t message_queue; /* Initialised elsewhere */

void client(void *arg) {
  void *message;
  /* ... */
  routines_send(message_queue, message);
  /* client blocks until message is received */
  /* ... */
}

void server(void *arg) {
  /* ... */
  /* server blocks until message is received */
  void *message = routines_wait(message_queue);
  /* client is now able to continue */
  /* ... */
}
```

### Asynchronous send

In an asynchronous send, the sending co-routine will continue after the
send occurs, even if no other co-routine has received the message. This
can act as a binary semaphore.

```c
routines_queue_t message_queue; /* Initialised elsewhere */

void client(void *arg) {
  void *message;
  /* ... */
  routines_signal(message_queue, message);
  /* client continues straight away */
  /* ... */
}

void server(void *arg) {
  /* ... */
  /* server blocks until message is received */
  void *message = routines_wait(message_queue);
  /* ... */
}
```

### Polling receive

A co-routine can check if any messages have been sent to a queue with
`routines_read` which will return the next message in the queue or
`NULL` otherwise. This allows for fully asynchronous message passing.

```c
routines_queue_t message_queue; /* Initialised elsewhere */

void client(void *arg) {
  void *message;
  /* ... */
  routines_signal(message_queue, message);
  /* client continues straight away */
  /* ... */
}

void server(void *arg) {
  /* ... */
  /* server will not block without a message */
  void *message = routines_read(message_queue);
  /* ... */
}
```

### Synchronised call

A client may also pass a queue on which a handler may send a reply
message. The client will block waiting for a message on this queue. 

```c
routines_queue_t message_queue; /* Initialised elsewhere */

void client(void *arg) {
  void *message;
  /* ... */
  routines_queue_t *reply_queue = routines_queue_create();
  void *reply = routines_call(message_queue, message, reply_queue);
  /* Client waits for reply on reply_queue */
  /* ... */
}

void server(void *arg) {
  /* ... */
  routines_queue_t *reply_queue;
  void *message = routines_recv(message_queue, &reply_queue);
  void *reply;
  /* ... */
  routines_signal(reply_queue, reply);
}
```

Note that the server in this example replied using `routines_signal`.
This prevents it from blocking in wait for the client to receive the
message. If a blocking send such as `routines_send` or `routines_call`
were used, the server would not be able to continue until the client
received the message or replied, respectively.

Application Programming Interface
---------------------------------

### Co-routines

#### `routines_spawn`

```c
routines_coroutine_t *routines_spawn(
    routines_task_t task,
    void *arg
);
```

Spawn a new co-routine which calls `task`, a pointer to a function
of type `void task(void *)`, and passes the second argument.

The return value is the created co-routine.

#### `routines_destroy`

```c
void routines_destroy(routines_coroutine_t *coroutine);
```

Destroy a co-routine object. If the co-routine was blocked in any
queues, it is removed from those queues. Any co-routines waiting to
_join_ the passed co-routine are resumed.

#### `routines_self`

```c
routines_coroutine_t *routines_self(void);
```

Returns the currently executing co-routine object.

#### `routines_state`

```c
routines_state_t routines_state(routines_coroutine_t *coroutine);
```

Returns the state of the given co-routine as one of the following:

  * `ROUTINES_COMPLETED` - the task function completed,
  * `ROUTINES_SUSPENDED` - the co-routine was manually suspended,
  * `ROUTINES_RUNNING` - the co-routine either running or waiting to
    run,
  * `ROUTINES_BLOCKED_SEND` - the co-routine is blocked waiting to send,
  * `ROUTINES_BLOCKED_RECV` - the co-routine is blocked waiting to
    receive, or
  * `ROUTINES_BLOCKED_JOIN` - the co-routine is blocked waiting for
    another co-routine to complete,

#### `routines_data_set`

```c
void routines_data_set(routines_coroutine_t *coroutine, void *data);
```

Associate some user data with a given co-routine.

#### `routines_data`

```c
void *routines_data(routines_coroutine_t *coroutine);
```

Get the associated user data for a given co-routine.

#### `routines_self_data_set`

```c
void routines_self_data_set(void *data);
```

Set the associated user data for the currently executing co-routine.

#### `routines_self_data`

```c
void *routines_self_data(void);
```

Access the associated user data for the currently executing co-routine.

### Scheduling

#### `routines_yeild`

```c
void routines_yield(void);
```

Return execution to another co-routine in a round-robin ordering.
If called from the initial process thread, this will resume any
co-routines that are now available to run.

#### `routines_join`

```c
void routines_join(routines_coroutine_t *coroutine);
```

This can only be called from within a co-routine.

Suspend the calling co-routine until the co-routine specified
in the argument completes or is destroyed.

#### `routines_suspend`

```c
void routines_suspend(routines_coroutine_t *coroutine);
```

Manually suspend a co-routine.

If the co-routine is blocked on any queues it is removed from those
queues. Any blocking messages are still sent.

When the co-routine is resumed, if it was waiting to receive a message
it will receive a NULL message with a NULL message queue.

#### `routines_suspend_self`

```c
void routines_suspend_self(void);
```

Suspend the currently executing co-routine.

#### `routines_resume`

```c
void routines_resume(routines_coroutine_t *coroutine);
```

Manually resume a co-routine.

If the co-routine is blocked on any queues it is removed from those
queues. If it was waiting to receive a message it will receive a NULL
message with a NULL message queue. Any blocking messages are still sent.

### Message passing & synchronisation

#### `routines_queue_create`

```c
routines_queue_t *routines_queue_create(void);
```

Create a new message queue for message passing and synchronisation.

#### `routines_queue_destroy`

```c
void routines_queue_destroy(routines_queue_t *queue);
```

Destroy a message queue, resuming any co-routines blocked waiting.

#### `routines_send` (blocking send)

```c
void routines_send(routines_queue_t *queue, void *message);
```

Send a message to a message queue, blocking until the message is
received.

#### `routines_wait` (blocking receive)

```c
void *routines_wait(routines_queue_t *queue);
```

Wait on a message queue until a message is available.

#### `routines_signal` (non-blocking send)

```c
void routines_signal(routines_queue_t *queue, void *message);
```

Send a message to a message queue without waiting for the message to be
received.

#### `routines_read` (non-blocking receive)

```c
void *routines_read(routines_queue_t *queue);
```

Read a message from the message queue (returns NULL if no message is in
the message queue).

#### `routines_call` (synchronising call)

```c
void *routines_call(
	routines_queue_t *send_queue,
	void *message,
	routines_queue_t *reply_queue
);
```

Send a message to a message queue and block waiting for a reply on
another message queue.

#### `routines_recv` (synchronising receive)

```c
void *routines_recv(
	routines_queue_t *recv_queue,
	routines_queue_t **reply_queue
);
```

Receive a message from a message queue and a message queue on which the
caller is expecting a reply.

#### `routines_post` (non-blocking call)

```c
void routines_post(
	routines_queue_t *send_queue,
	void *message,
	routines_queue_t *reply_queue
);
```

Send a message to a message queue along with a message queue on which a
reply should later be sent.
