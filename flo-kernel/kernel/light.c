#include <linux/light.h>
#include <linux/syscalls.h>
#include <linux/list.h>
#include <linux/wait.h>

static struct light_intensity k_li = {
    .cur_intensity = 0;
};

static struct list_head *events = NULL;
#define get_event(struct list_head *v) \
    container_of(v, struct ev, ev_h);

static struct light_intesity li_buf[WINDOW];
static int curr = 0;
static int nr_readings = 0;

/* Working out a locking strategy:
 * - Set() accesses k_li.
 * - Get() accesses k_li.
 * - Create() accesses events (w).
 * - Signal() acceses k_li (w) and li_buf (rw), and events (r).
 * - Wait() acceses events (r), events[id] (w): ev.queue.
 * - Destroy() accesses events (w).
 *
 * Priorities:
 * - Destroy (events lock: prioritize the write operation).
 * - Signal (li_buf lock:   first writes then reads; concurrency/SMP an
    issue. Prioritize reading to allow previous call to complete if
    signal() acquires lock twice - else doesn't matter since signal()
    is the only function accessing li_buf.
             k_li lock:     prioritize the write operation).
 * - Wait (Destroy/Signal both prioritize the write operation).
 * - Create (Again, writing is prioritized).
 *
 * Interrupts:
 * - Destroy should be able to interrupt awakening of queued processes
 *  upon signal. Do not disable signal_event interrupts while/after
 *  signal reads events (abort). It does not matter whether li_buf rw
 *  operations are interruptible on the processor of an event destroy.
 * - Destroy should also be able to interrupt queuing up of processes
 *  on a certain queue (abort).
 * - Wait/Create cannot interrupt signal, etc.
 * To cover all cases define a rule for each pair of functions. This
 * just gives a framework of thought.
 *
 */
DEFINE_SPINLOCK(li_lock);
DEFINE_SPINLOCK(ev_lock);
DEFINE_SPINLOCK(bf_lock);

/*
 * Set current ambient intensity in the kernel.
 *
 * The parameter user_light_intensity is the pointer to the address
 * where the sensor data is stored in user space. Follow system call
 * convention to return 0 on success and the appropriate error value
 * on failure.
 *
 * syscall number 378
 */
SYSCALL_DEFINE1(set_light_intensity, struct light_intensity __user *,
    user_light_intensity)
{
    if (get_current_user()->uid != 0)
        return -EACCES;

    if (user_light_intensity == NULL)
        return -EINVAL;

    if (user_light_intensity->cur_intensity <= 0 ||
        user_light_intensity->cur_intensity > MAX_LI)
        return -EINVAL;


    spin_lock(&li_lock);
    if (copy_from_user(&k_li, user_light_intensity, 
            sizeof(struct light_intensity))) {

        spin_unlock(&li_lock);
        return -EFAULT;
    }

    spin_unlock(&li_lock);
    return 0;
}

/*
 * Retrieve the scaled intensity set in the kernel.
 *
 * The same convention as the previous system call but
 * you are reading the value that was just set.
 * Handle error cases appropriately and return values according
 * to convention.
 * The calling process should provide memory in userspace
 * to return the intensity.
 *
 * syscall number 379
 */
SYSCALL_DEFINE1(get_light_intensity, struct light_intensity __user *,
    user_light_intensity)
{
    if (!user_light_intensity)
        return -EINVAL;

    spin_lock(&li_lock);
    if (copy_to_user(user_light_intensity, &k_intensity,
            sizeof(struct light_intensity))) {

        spin_unlock(&li_lock);
        return -EFAULT;
    }

    spin_unlock(&li_lock);
    return 0;
}

/*
 * Create an event based on light intensity.
 *
 * If frequency exceeds WINDOW, cap it at WINDOW.
 * Return an event_id on success and the appropriate error on failure.
 *
 * system call number 380
 */
SYSCALL_DEFINE1(light_evt_create, struct event_requirements *, intensity_params)
{
    int retval = 0;
    struct ev *ev;

    if (!intensity_params)
        return -EINVAL;

    ev = kmalloc(sizeof(*ev), GFP_KERNEL);

    if (!ev)
        return -ENOMEM;

    if (copy_from_user(&ev->reqs, intensity_params,
        sizeof(struct event_requirements))) {
        kfree(new_event);
        return -EFAULT;
    }

    if (events)
        ev->id = get_event(events)->id + 1;
    else
        ev->id = 1;
    retval = ev->id;
    LIST_HEAD_INIT(ev->ev_h);
    DECLARE_WAITQUEUE(queue);

    spin_lock(&ev_lock);
    if (events)
        list_add_tail(ev, events);
    events = ev;
    spin_unlock(&ev_lock);

    /* Could ev be destroyed right before execution of the next line?
     * The Linux scheduler is preemptive and hardware interrupts are
     * possible. Let's not risk it.
     */
    return retval;
}

static inline struct list_head *search_event_by_id(int event_id)
{
    struct list_head *v = events;

    if (!v)
        return ERR_PTR(-EFAULT);

    spin_lock(&ev_lock); /* Reading (i.e. reader) must be exclusive */

    while(get_event(v)->id != event_id) {
        v = v->next;

        if (v == events) {
            spin_unlock(&ev_lock);
            return ERR_PTR(-EINVAL); /* Iterated through entire list */

        } else if (!v) {
            printk(KERN_CRIT "ERROR LIGHT EVENTS: LIST CORRUPTION.\n");
            spin_unlock(&ev_lock);
            return ERR_PTR(-EFAULT);
        }
    }
    return v;
}

/*
 * Destroy an event using the event_id.
 *
 * Return 0 on success and the appropriate error on failure.
 *
 * system call number 383
 */
SYSCALL_DEFINE1(light_evt_destroy, int, event_id)
{
    struct ev *ev;
    struct list_head *v = search_event_by_id(event_id);

    if (IS_ERR(v))
        return PTR_ERR(v);

    list_del(v);
    spin_unlock(&ev_lock);
    /* The event has been removed from the "global" list and can no
     * longer be accessed by other callers. Next and prev have been
     * been put off but the pointer value itself is not changed.
     */

    ev = get_event(v);
    ev->destroy = 1; /* True */
    wake_up_all(&ev->queue);
    return 0;
}

static inline int do_wait(struct ev *ev, wait_queue_head_t *w)
{
    int retval = 1;

    while (ev->no_satisfaction && !ev->destroy) {
        prepare_to_wait(&ev->queue, w, TASK_INTERRUPTIBLE);
        if (retval)
            retval = 0;

        if (signal_pending(current)) {
            retval = -EINTR;
            break;
        }

        spin_unlock(&ev_lock); /* Unlock before sleep */
        schedule();
        spin_lock(&ev_lock); /* Lock while reading event */
    }
    if (retval < 1) {
        finish_wait(&ev->queue, w); /* Dequeue */
    }
    if (ev->destroy)
        kfree(ev); /* Destroy */

    spin_unlock(&ev_lock);
    return retval;
}

/*
 * Block a process on an event.
 *
 * It takes the event_id as parameter. The event_id requires verification.
 * Return 0 on success and the appropriate error on failure.
 *
 * system call number 381
 */
SYSCALL_DEFINE1(light_evt_wait, int, event_id)
{
    struct list_head *v = search_event_by_id(event_id);

    if (IS_ERR(v))
        return PTR_ERR(v);

    /* New wait queue entry - works just like list_head. */
    DEFINE_WAIT(wait);

    return do_wait(get_event(v), &wait);
}


/* The kernel has a call table for syscalls in which all these
 * inline functions can fit nicely.
 */
static inline int do_count(struct event_requirements *reqs)
{
    int i = 0, reading, count = reqs->frequency;
    for ( ; i > -nr_readings; --i) {

        if (curr + i < 0)
            reading = li_buf[curr + i + WINDOW].cur_intensity;
        else
            reading = li_buf[curr + i].cur_intensity;

        if (reading > reqs->req_intensity - NOISE) {
            if (!--count)
                return 1;
        }
    }
    /* We could also update largest_event_frequency's req_intensity
     * and largest_event_req_intensity's frequency here, if either
     * have larger "counterparts".
     */
    return 0;
}

static inline void update_event_stats(void)
{
    int signal_event = 0, retval = 0;
    struct list_head *v = events;
    struct ev *ev;
    struct event_requirements r;

    /* A minor optimization */
    int largest_event_frequency = 0;
    int its_req_intensity = 0;
    int largest_event_req_intensity = 0;
    int its_frequency = 0;

    spin_lock(&ev_lock);

    while (1) {
        ev = get_event(v);
        r = ev->reqs;

        if (r.req_intensity <= largest_event_req_intensity  &&
            r.frequency     <= its_frequency                ||
            r.frequency     <= largest_event_frequency      &&
            r.req_intensity <= its_req_intensity)
            signal_event = 1;
        else
            signal_event = do_count(&r);

        if (signal_event) {

            ev->no_satisfaction = 0;
            wake_up_all(&ev->queue);

            if (r.req_intensity <=  largest_event_req_intensity &&
                r.frequency     >   its_frequency)

                its_frequency = r.frequency;

            if (r.frequency     <=  largest_event_frequency     &&
                r.req_intensity >   its_req_intensity)

                its_req_intensity = r.req_intensity;
        }

        v = v->next;
        if (v == events)
            break;
    }
}

/* Do not use without holding the li_lock.
 */
static inline int update_buffer(void)
{
    int retval = 0;
    spin_lock(&bf_lock);

    /* How easy would it be to turn this into a fixed size BST */
    curr = (curr + 1) % WINDOW;
    li_buf[curr] = k_li.cur_intensity;

    if (nr_readings < WINDOW-1)
        ++nr_readings;

    retval = update_event_stats();
    /* We want to make sure to update event stats at least once every
        WINDOW light intensity updates. Otherwise, we may be missing out
        on events. A minimum frequency of 1 is allowed, and in that
        case, it is optimal to update stats at every light intensity
        update. Otherwise, we can use a semaphore initialized to
        <= WINDOW to update the buffer, ignoring race conditions between
        event stats (that may miss out on signaling an event) and buffer
        updates. This is a tradeoff.
     */

    spin_unlock(&bf_lock);
    return retval;
}

/*
 * The light_evt_signal system call.
 *
 * Takes sensor data from user, stores the data in the kernel,
 * and notifies all open events whose
 * baseline is surpassed.  All processes waiting on a given event 
 * are unblocked.
 *
 * Return 0 success and the appropriate error on failure.
 *
 * system call number 382
 */
SYSCALL_DEFINE1(light_evt_signal, struct light_intensity __user *,
    user_light_intensity)
{
    struct list_head *v = events;
    int retval = 0;

    if (get_current_user()->uid != 0)
        return -EACCES;

    if (user_light_intensity == NULL)
        return -EINVAL;

    if (user_light_intensity->cur_intensity <= 0 ||
        user_light_intensity->cur_intensity > MAX_LI)
        return -EINVAL;

    spin_lock(&li_lock);
    if (copy_from_user(&k_li, user_light_intensity, 
            sizeof(struct light_intensity))) {

        spin_unlock(&li_lock);
        return -EFAULT;
    }

    retval = update_buffer();
    spin_unlock(&li_lock);

    return retval;
}
