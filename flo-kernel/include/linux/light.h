#ifndef _LIGHT_H
#define _LIGHT_H

#define MAX_LI  3276800
#define NOISE   20
#define WINDOW  20

/*
    Use this wrapper to pass the intensity to your system call
*/
struct light_intensity {
    int cur_intensity;
};

/*
    An event triggers if:
    1.  A sensor reading of > 'req_intensity' - NOISE occurs,
    2.  at least 'frequency' number of times in the TIME_INTERVAL
        defined in userspace. Only the user passing on frequency
        to the kernel knows TIME_INTERVAL. The kernel does not
        care about this value.
*/
struct event_requirements {
    int req_intensity;
    int frequency;
};

/*
    Data members for an event object:
    struct ev {
        .id: id allocated to this user-created event (in FI order)
        .reqs: event_requirements for this user-created event; thresholds
            that make it trigger, hence waking all processes waiting
            on this event
        .head: list_head entry, in "global" list of user-created events,
            also used as iterator
        .queue: wait_queue_head entry, in the wait queue dedicated to this
            event, of all the userspace processes waiting to be woken
            on this event
    }
*/
struct ev {
    int id;
    struct event_requirements reqs;
    struct list_head ev_h;
    wait_queue_head_t queue;
};

#endif
