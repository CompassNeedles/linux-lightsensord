#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <wait.h>

#include "light.h"

#define TIME_INTERVAL 1000000
#define LATENT_PERIOD 500000

#define __NR_get_light_intensity    379
#define __NR_light_evt_create       380
#define __NR_light_evt_wait         381
#define __NR_light_evt_signal       382
#define __NR_light_evt_destroy      383

int get(struct light_intensity *kernel_light_intensity)
{
	return syscall(__NR_get_light_intensity, kernel_light_intensity);
}

int light_evt_create(struct event_requirements *intensity_params)
{
	return syscall(__NR_light_evt_create, intensity_params);
}

int light_evt_wait(int event_id)
{
	return syscall(__NR_light_evt_wait, event_id);
}

int light_evt_signal(struct light_intensity *user_light_intensity)
{
	return syscall(__NR_light_evt_signal, user_light_intensity);
}

int light_evt_destroy(int event_id)
{
	return syscall(__NR_light_evt_destroy, event_id);
}

/* Test:
 * Quick (and dirty...)... Lacks unit tests and edge cases.
 */
int main (void)
{
	int child_pid, read, evt_id;
	struct light_intensity cur_value;


	/* Define events and have parent create them in kernel */
	struct event_requirements A = {500000, 5};
	struct event_requirements B = {50000, 7};
	struct event_requirements C = {500, 3};

	int A_id = light_evt_create(&A);
	if (A_id <= 0) {
		printf("Error[%i] A: %s.\n", A_id, strerror(errno));
	} else {
		printf("Created evt_A_id[%i] for children 0-2 to wait on.\n", A_id);
	}

	int B_id = light_evt_create(&B);
	if (B_id <= 0) {
		printf("Error[%i] B: %s.\n", B_id, strerror(errno));
	} else {
		printf("Created evt_B_id[%i] for children 3-5 to wait on.\n", B_id);
	}

	int C_id = light_evt_create(&C);
	if (C_id <= 0) {
		printf("Error[%i] C: %s.\n", C_id, strerror(errno));
	} else {
		printf("Created evt_C_id[%i] for children 6-8 to wait on.\n", C_id);
	}


	/* Fork children */
	int N = 10;
	int n = 0;
	int e = 0;

	for ( ; n < N; ++n) {
		child_pid = fork();
		if (child_pid == 0) {
			usleep(LATENT_PERIOD);
			break;
		} else
			printf("Created child[%i] pid[%i].\n", n, child_pid);
	}


	/* Signal poll sensor data, wait on events, print and destroy */
	printf("PID[%i] executing.\n", getpid());

	while (child_pid == 0) {

		/* Periodically signal */
		if (n > 8) {

			read = usleep(TIME_INTERVAL);
			if (read)
				printf("Error[%i] usleep: %s.\n", read, strerror(errno));

			/* Poll sensor data from kernel.
			 * Have daemon run simultaneously. */
			read = get(&cur_value);
			if (read)
				printf("Error[%i]: get failure, %s.\n", read, strerror(errno));

			printf("Intensity in kernel: %i.\n", cur_value.cur_intensity);

			/* Signal to kernel */
			read = light_evt_signal(&cur_value);
			if (read)
				printf("Error[%i] signal: %s.\n", read, strerror(errno));

			continue; /* child [9] is only meant to signal */

		} else if (n > 5) {     /* 6,7,8 */
			evt_id = C_id;
		} else if (n > 2) {     /* 3,4,5 */
			evt_id = B_id;
		} else {                /* 0,1,2 */
			evt_id = A_id;
		}

		printf("Making wait on event id: %i.\n", evt_id);
		read = light_evt_wait(evt_id);

		if (read) {
			printf("Error[%i] wait on event: %s\n", read, strerror(errno));
			printf("Child[%i] pid[%i] evt_id[%i].\n", n, getpid(), evt_id);
			break; /* discontinue tests */
		} else {
			printf("Child successfully waited on event.\n");
			printf("Child[%i] pid[%i] evt_id[%i].\n", n, getpid(), evt_id);
			++e; /* increment event count */
		}

		/* At the 9th recorded event for children[0,3,6], destroy */
		if (e == 9 && ((n % 3) == 0)) {
			printf("Success: child[%i] pid [%i]: recorded 9 events\n",
				n, getpid());

			printf("         destroying evt_id[%i]...\n", evt_id);
			read = light_evt_destroy(evt_id);
			/* Print later - nest less */
		}

		if (e < 9 || (n % 3) != 0)
			continue; /* Do not destroy or display results; keep looping. */

		if (read == 0) {
			printf("         evt_id[%i] destroyed successfully.\n", evt_id);
			break;
		} else {
			printf("         Error[%i]: %s.\n", read, strerror(errno));
		}
		/* Manually sig-interrupt all other children processes. */
	}
	/* Wait for children or weird things happen with the command line */
	if (child_pid > 0) {
		n = -1;
		while (N--)
			wait(&n);
	}
	printf("PID[%i] exiting forever.\n", getpid());
	return 0;
	/* The process signaling remains. */
}
