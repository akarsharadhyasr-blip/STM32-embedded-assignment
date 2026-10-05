#ifndef RTOS_LOGGER_H
#define RTOS_LOGGER_H

// creates the queues + producer/consumer tasks, then runs the consumer in the
// calling (default) task. never returns. producer source is the EXTI callback.
void logger_start(void);

#endif
