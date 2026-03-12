#ifndef MORSE_H
#define MORSE_H

#include <linux/mutex.h>

#define SIZE 256

// Shared variables (declared as extern)
extern char buffer[SIZE];
extern int head;
extern int tail;
extern struct mutex buffer_mutex;

// Function to initialize proc
int morse_proc_init(void);
// Function to cleanup proc
void morse_proc_cleanup(void);

#endif
