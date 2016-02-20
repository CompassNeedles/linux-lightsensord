#ifndef _LIGHT_H
#define _LIGHT_H

/*
 * Use this wrapper to pass the intensity to your system call
 */

struct light_intensity{
	int cur_intensity;
};

/*
 * Syscall wrapper functions
 */

int get(struct light_intensity* user_light_intensity);
int set(struct light_intensity* user_light_intensity);

#endif
