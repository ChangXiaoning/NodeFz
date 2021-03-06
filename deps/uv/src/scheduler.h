#ifndef UV_SRC_SCHEDULER_H_
#define UV_SRC_SCHEDULER_H_

/* 
 * -----------------------------------------
 *    Introduction
 * -----------------------------------------
 *
 * We want to be able to try out different schedulers so that we can compare their performance.
 * Our scheduler design is therefore modular.
 * The scheduler implementation is chosen at runtime during initialization.
 *
 * This file accomplishes two things:
 *   1. It defines the public interface for the libuv scheduler.
 *   2. It defines the function typedefs for a scheduler implementation,
 *      indicating the functions each scheduler implementation must offer.
 *
 * Implementation details:
 *   Some pieces of the "public scheduler" are shared by all scheduler implementations.
 *   For the rest, the "public scheduler" defers the work to the "private scheduler".
 *   During initialization, the "public scheduler" gets function pointers for the portions that it does not
 *   implement itself from the "private scheduler".
 *
 * -----------------------------------------
 *    Scheduler overview
 * -----------------------------------------
 *
 * libuv makes use of multiple threads (looper thread and TP threads).
 * On unix, the pthreads library decides when each thread gets scheduled in terms of a generic scheduling quantum.
 * We add our own scheduler that can make decisions at the libuv semantic level.
 *
 * Our scheduler requires threads to call into it at "interesting points".
 * These call points are called "schedule points" and are indicated to the scheduler via scheduler_thread_yield().
 *
 *  Thread type         Sensitive activities (schedule points)
 * -------------------------------------------------------------------------------------------
 *    L, TP                Before/after executing a CB
 *    TP                   Before/after taking the next "Work" item from the work queue
 *    TP                   Before/after placing a completed "Work" item into the done queue
 *
 * Schedulers can make scheduling decisions based on a few things:
 *   - thread ID 
 *   - thread type
 *   - in the case of SCHEDULE_POINT_BEFORE_EXEC_CB, the CB that will be executed
 *   - ...
 *
 * To offer flexibility, scheduler_thread_yield takes a void *schedule_point_details whose contents depend on the schedule point.
 * That way, if we introduce a new scheduler that wants other details, we just have to change the underlying struct for the schedule point.
 *
 * Schedulers have two modes: record and replay.
 *  Record:
 *    The scheduler doesn't influence the actions of a thread much, but it does record the sequence of
 *    activities for subsequent replay.
 *  Replay:
 *    Attempt to reproduce a previously-recorded execution.
 *    Can also be used to "follow a script" that may not have been previously recorded, e.g. via a rescheduler.
 *    Must be invoked with the same inputs.
 *    If the input schedule is identical to a previously-recorded schedule, it must be achievable.
 *    If it's not identical to a previously-recorded schedule, it may (given identical external inputs (e.g. gettimeofday, read(), etc.)) be achievable.
 *      If the input schedule is a variation of the recorded schedule (e.g. switching the order of two callbacks), 
 *      one or more of the changes in the re-ordering may produce a different logical structure. 
 *
 * For research purposes, the system is designed to support multiple scheduler implementations.
 * The libuv code should initialize the scheduler with scheduler_init(), designating the desired scheduler type and mode.
 * It should then call the appropriate scheduler_* APIs at "schedule points".
 * The scheduler will direct the API to the designated scheduler.
 * This paradigm is a "dispatch table", like the Linux VFS system.
 *
 * -----------------------------------------
 *    To implement a new scheduler
 * -----------------------------------------
 *
 * scheduler.h: Update scheduler_type_t with the new type.
 * scheduler.c: Update scheduler_init for the new type.
 * Create new scheduler_X.[ch] files to define each of the schedulerImpl_* APIs declared below
 *   (simply copy an existing scheduler implementation's skeleton).
 *
 * -----------------------------------------
 *    To add a new schedule point
 * -----------------------------------------
 *
 * scheduler.h: Update schedule_point_t with the new type.
 *              Declare new spd_ struct.
 * scheduler.c: Update global arrays and to_string methods.
 *              Update schedulerImpl_thread_yield for the relevant schedulers.
 *              If the associated spd_ struct requires output from scheduler, fill in the default
 *                in the yield for any non-relevant schedulers.
 */

#include "unified-callback-enums.h"
#include "logical-callback-node.h"

#include <uv.h>
#include "uv-common.h"

/* The different scheduler types we support. */
enum scheduler_type_e
{
  SCHEDULER_TYPE_MIN,

  SCHEDULER_TYPE_VANILLA = SCHEDULER_TYPE_MIN,
  SCHEDULER_TYPE_CBTREE,
  SCHEDULER_TYPE_FUZZING_TIME,
  SCHEDULER_TYPE_TP_FREEDOM,

  SCHEDULER_TYPE_MAX = SCHEDULER_TYPE_TP_FREEDOM
};
typedef enum scheduler_type_e scheduler_type_t;
const char * scheduler_type_to_string (scheduler_type_t type);

/* The mode in which the scheduler is to run. */
enum scheduler_mode_e
{
  SCHEDULER_MODE_MIN,

  SCHEDULER_MODE_RECORD = SCHEDULER_MODE_MIN,
  SCHEDULER_MODE_REPLAY,

  SCHEDULER_MODE_MAX = SCHEDULER_MODE_REPLAY
};
typedef enum scheduler_mode_e scheduler_mode_t;
const char * scheduler_mode_to_string (scheduler_mode_t mode);

/* The types of threads of which the scheduler is aware. */
enum thread_type_e
{
  THREAD_TYPE_MIN,

  THREAD_TYPE_LOOPER = THREAD_TYPE_MIN,
  THREAD_TYPE_THREADPOOL,

  THREAD_TYPE_MAX = THREAD_TYPE_THREADPOOL
};
typedef enum thread_type_e thread_type_t;
const char * thread_type_to_string (thread_type_t mode);

/* The different schedule points. */
enum schedule_point_e
{
  SCHEDULE_POINT_MIN,

  /* Schedule points reached by TP and Looper threads. */
  SCHEDULE_POINT_BEFORE_EXEC_CB = SCHEDULE_POINT_MIN, /* Either type: invoke_callback, just before cbi_execute_callback(). */
  SCHEDULE_POINT_AFTER_EXEC_CB, /* Either type: invoke_callback, just after cbi_execute_callback(). */

  /* Looper schedule points. */
  SCHEDULE_POINT_LOOPER_BEFORE_EPOLL, /* LOOPER: uv__io_poll, before epoll_wait(). */
  SCHEDULE_POINT_LOOPER_AFTER_EPOLL,  /* LOOPER: uv__io_poll, after epoll_wait(). */

  SCHEDULE_POINT_LOOPER_IOPOLL_BEFORE_HANDLING_EVENTS, /* LOOPER: uv__io_poll, before handling the events returned by epoll_wait(). */

  SCHEDULE_POINT_LOOPER_GETTING_DONE, /* LOOPER: uv__work_done, just before getting item from private done queue. */

  SCHEDULE_POINT_LOOPER_RUN_CLOSING, /* LOOPER: In uv__run_closing_handles, deciding whether to continue or stop. */

  /* Timer schedule points (also run by LOOPER). */
  SCHEDULE_POINT_TIMER_READY, /* Timer: I'm in uv__ready_timers considering a pending timer. */
  SCHEDULE_POINT_TIMER_RUN, /* Timer: I'm in uv__run_timers considering the set of ready timers. */
  SCHEDULE_POINT_TIMER_NEXT_TIMEOUT, /* Timer: I'm in uv__next_timeout being asked how long until the next timer goes off. */

  /* TP schedule points. */
  SCHEDULE_POINT_TP_WANTS_WORK, /* TP: worker, when wq is locked and non-empty. */

  SCHEDULE_POINT_TP_GETTING_WORK,  /* TP: worker, when about to retrieve an item from locked and non-empty wq. */
  SCHEDULE_POINT_TP_GOT_WORK, /* TP: worker, after retrieving a work item. */

  SCHEDULE_POINT_TP_BEFORE_PUT_DONE, /* TP: worker, before placing done item in done queue. */
  SCHEDULE_POINT_TP_AFTER_PUT_DONE, /* TP: worker, after placing done item in done queue. */
 
  SCHEDULE_POINT_MAX = SCHEDULE_POINT_TP_AFTER_PUT_DONE
};
typedef enum schedule_point_e schedule_point_t;

const char * schedule_point_to_string (schedule_point_t point);

struct shuffleable_items_s
{
  size_t item_size; /* Size of each item in bytes. */

  unsigned nitems;  /* Number of entries in the arrays. */
  void *items;      /* Array of nitems items. */
  int *thoughts;    /* Array of nitems "thoughts", one thought per item. */ 
};
typedef struct shuffleable_items_s shuffleable_items_t;

/* The Schedule Point Details (SPD) provided for each schedule point. 
 * There's an SPD_X_t for each schedule_point_t. Use them together.
 * An SPD can include both input fields (guidance to scheduler) and output fields (guidance from scheduler).
 * If an SPD requires output, all schedulers should fill it in appropriately.
 */
struct spd_before_exec_cb_s
{
  int magic;

  enum callback_type cb_type;
  lcbn_t *lcbn; /* NOT USED. NULL for non-logical CBs. */
};
typedef struct spd_before_exec_cb_s spd_before_exec_cb_t;

void spd_before_exec_cb_init (spd_before_exec_cb_t *spd_before_exec_cb);
/* Returns non-zero if valid. */
int spd_before_exec_cb_is_valid (spd_before_exec_cb_t *spd_before_exec_cb);

/* {before,after}_exec_cb share the same structure. */
typedef spd_before_exec_cb_t spd_after_exec_cb_t;

void spd_after_exec_cb_init (spd_after_exec_cb_t *spd_after_exec_cb);
/* Returns non-zero if valid. */
int spd_after_exec_cb_is_valid (spd_after_exec_cb_t *spd_after_exec_cb);

struct spd_before_epoll_s
{
  int magic;
};
typedef struct spd_before_epoll_s spd_before_epoll_t;

void spd_before_epoll_init (spd_before_epoll_t *spd_before_epoll);
/* Returns non-zero if valid. */
int spd_before_epoll_is_valid (spd_before_epoll_t *spd_before_epoll);

struct spd_after_epoll_s
{
  int magic;
};
typedef struct spd_after_epoll_s spd_after_epoll_t;

void spd_after_epoll_init (spd_after_epoll_t *spd_after_epoll);
/* Returns non-zero if valid. */
int spd_after_epoll_is_valid (spd_after_epoll_t *spd_after_epoll);

struct spd_iopoll_before_handling_events_s
{
  int magic;

  /* nitems:   INPUT         The number of events.
   * items:    INPUT/OUTPUT  Array of events returned by epoll_wait. Scheduler may shuffle them.
   * thoughts: OUTPUT        1 means handle the corresponding entry in items, 0 means defer.
   */
  shuffleable_items_t shuffleable_items;
};
typedef struct spd_iopoll_before_handling_events_s spd_iopoll_before_handling_events_t;

void spd_iopoll_before_handling_events_init (spd_iopoll_before_handling_events_t *spd_iopoll_before_handling_events);
/* Returns non-zero if valid. */
int spd_iopoll_before_handling_events_is_valid (spd_iopoll_before_handling_events_t *spd_iopoll_before_handling_events);

struct spd_wants_work_s
{
  int magic;
  struct timespec start_time; /* INPUT. When did we start wanting work? */
  QUEUE *wq; /* INPUT. A non-empty wq. Caller must ensure mutex. */
  int should_get_work; /* OUTPUT. Whether or not to proceed to SCHEDULE_POINT_TP_GETTING_WORK. 1 means "proceed". */
};
typedef struct spd_wants_work_s spd_wants_work_t;

void spd_wants_work_init (spd_wants_work_t *spd_wants_work);
/* Returns non-zero if valid. */
int spd_wants_work_is_valid (spd_wants_work_t *spd_wants_work);

struct spd_getting_work_s
{
  int magic;
  QUEUE *wq; /* INPUT. A non-empty wq. Caller must ensure mutex. */
  int index; /* OUTPUT. The index to choose. A choice of 0 means 'treat wq as FIFO'. */
};
typedef struct spd_getting_work_s spd_getting_work_t;

void spd_getting_work_init (spd_getting_work_t *spd_getting_work);
/* Returns non-zero if valid. */
int spd_getting_work_is_valid (spd_getting_work_t *spd_getting_work);

struct spd_got_work_s
{
  int magic;
  struct uv__work *work_item;
  int work_item_num; /* What entry in wq was this? Starts at 0. */
};
typedef struct spd_got_work_s spd_got_work_t;

void spd_got_work_init (spd_got_work_t *spd_got_work);
/* Returns non-zero if valid. */
int spd_got_work_is_valid (spd_got_work_t *spd_got_work);

/* {before,after}_put_done share the same structure. */
typedef spd_got_work_t spd_before_put_done_t;

void spd_before_put_done_init (spd_before_put_done_t *spd_before_put_done);
/* Returns non-zero if valid. */
int spd_before_put_done_is_valid (spd_before_put_done_t *spd_before_put_done);

typedef spd_got_work_t spd_after_put_done_t;

void spd_after_put_done_init (spd_after_put_done_t *spd_after_put_done);
/* Returns non-zero if valid. */
int spd_after_put_done_is_valid (spd_after_put_done_t *spd_after_put_done);

/* getting_{work,done} share the same structure. */
typedef spd_getting_work_t spd_getting_done_t;

void spd_getting_done_init (spd_getting_done_t *spd_getting_done);
/* Returns non-zero if valid. */
int spd_getting_done_is_valid (spd_getting_done_t *spd_getting_done);

struct spd_looper_run_closing_s
{
  int magic;

  int defer; /* OUTPUT: Set to 1 if we should defer the next handle and all subsequent handles until the next turn of the loop, 0 to defer. */
};
typedef struct spd_looper_run_closing_s spd_looper_run_closing_t;

void spd_looper_run_closing_init (spd_looper_run_closing_t *spd_looper_run_closing);
/* Returns non-zero if valid. */
int spd_looper_run_closing_is_valid (spd_looper_run_closing_t *spd_looper_run_closing);

struct spd_timer_ready_s
{
  int magic;

  uv_timer_t *timer; /* INPUT: A pending timer. */
  uint64_t now;      /* INPUT: Current loop->time. */
  int ready;         /* OUTPUT: Set to 1 if the timer is ready, else 0. */
};
typedef struct spd_timer_ready_s spd_timer_ready_t;

void spd_timer_ready_init (spd_timer_ready_t *spd_timer_ready);
/* Returns non-zero if valid. */
int spd_timer_ready_is_valid (spd_timer_ready_t *spd_timer_ready);

struct spd_timer_run_s
{
  int magic;

  /* nitems:   INPUT         The number of timers.
   * items:    INPUT/OUTPUT  Array of ready uv_timer_t *'s. Scheduler may shuffle them.
   * thoughts: OUTPUT        1 means run the corresponding entry in items, 0 means defer.
   */
  shuffleable_items_t shuffleable_items;
};
typedef struct spd_timer_run_s spd_timer_run_t;

void spd_timer_run_init (spd_timer_run_t *spd_timer_run);
/* Returns non-zero if valid. */
int spd_timer_run_is_valid (spd_timer_run_t *spd_timer_run);

struct spd_timer_next_timeout_s
{
  int magic;
  uv_timer_t *timer; /* INPUT: The next-scheduled timer. */
  uint64_t now; /* INPUT: Current loop->time. */
  uint64_t time_until_timer; /* OUTPUT: How long until timer "will" go off, relative to loop->time? Not an ironclad prediction, just a recommendation to uv__io_poll. */
};
typedef struct spd_timer_next_timeout_s spd_timer_next_timeout_t;

void spd_timer_next_timeout_init (spd_timer_next_timeout_t *spd_timer_next_timeout);
/* Returns non-zero if valid. */
int spd_timer_next_timeout_is_valid (spd_timer_next_timeout_t *spd_timer_next_timeout);


/* Returns non-zero if the {point, pointDetails} combination is valid. */
int schedule_point_looks_valid (schedule_point_t point, void *pointDetails);

/********************************
 * "Public" scheduler methods intended for use throughout libuv.
 ********************************/

/* Call this prior to any other scheduler_* routines. 
 *   type: What type of scheduler to use?
 *   mode: What mode in which to use it? Not all schedulers support all modes.
 *   schedule_file: In RECORD mode, where to put the schedule we record.
 *                  In REPLAY mode, where to find the schedule we wish to replay. 
 *   args: Depends on type. Consult the header file for the scheduler implementation.
 */
void scheduler_init (scheduler_type_t type, scheduler_mode_t mode, char *schedule_file, void *args);

/* Register the calling thread under the specified type. 
 * Each thread should call this while it is initializing. 
 * Once set, a thread's type should not change.
 */
void scheduler_register_thread (thread_type_t type);

/* Register LCBN for potential scheduler_execute_lcbn()'d later. 
 * Caller must ensure mutex for deterministic replay.
 */
void scheduler_register_lcbn (lcbn_t *lcbn);

/* REPLAY mode. 
 * Returns the callback_type of the next scheduled LCBN.
 * If scheduler has diverged, returnes CALLBACK_TYPE_ANY.
 * This allows uv__run to repeat loop stages if waiting for input or a timer.
 */
enum callback_type scheduler_next_lcbn_type (void);

/* Thread yields at a schedule point, allowing the scheduler to make a decision.
 * Call before doing or after doing something "sensitive", as described in the scheduler documentation.
 * This gives the scheduler the opportunity to make a decision.
 *   RECORD mode: might make a random choice about who goes next
 *   REPLAY mode: lets us have reproducible results
 *
 * For points SCHEDULE_POINT_{BEFORE,AFTER}_EXEC_CB, ensures mutex during execution of CB.
 *
 * INPUT:         point: What state is the calling thread in?
 * INPUT/OUTPUT:  schedule_point_details: The spd_X associated with the point.
 *                  The scheduler may give advice in schedule_point_details.
 */
void scheduler_thread_yield (schedule_point_t point, void *schedule_point_details);

/* Returns the thread id of the thread currently executing a CB, or NO_CURRENT_CB_THREAD.
 * Only thread-safe if the calling thread is the one currently executing a CB.
 * This facilitates a clean release in the exit() path:
 *   while (uv_thread_self() == scheduler_current_cb_thread())
 *   {
 *     ...
 *     scheduler_thread_yield(SCHEDULE_POINT_AFTER_EXEC_CB, &spd_after_exec_cb);
 *   }
 * The loop is necessary because the exit() could be invoked at the top of a a stack of nested CBs.
 */
static const uv_thread_t NO_CURRENT_CB_THREAD = -1;
uv_thread_t scheduler_current_cb_thread (void);

/* Dump the schedule (whatever that means; depends on the scheduler implementation) to the schedule_file specified in schedule_init. 
 *   RECORD mode: duh
 *   REPLAY mode: we don't want to overwrite the input schedule, so we emit to sprintf("%s-replay", schedule_file). 
 * Returns the name of the output file.
 */
void scheduler_emit (void);

/* How many LCBNs from the input schedule have not been executed yet?
 * In RECORD mode, must return non-zero.
 */
int scheduler_lcbns_remaining (void);

/* REPLAY mode.
 * Returns non-zero if schedule has diverged, else 0. 
 */
int scheduler_schedule_has_diverged (void);

/* How many LCBNs have already been executed? 
 * This is measured by the number of times scheduler_thread_yield is called
 * at schedule point SCHEDULE_POINT_AFTER_EXEC_CB. */
long unsigned scheduler_n_executed (void);

/* RECORD vs. REPLAY mode may affect control-flow decisions. 
 * The scheduler mode is not a constant. We may shift from REPLAY to RECORD mode.
 */
scheduler_mode_t scheduler_get_scheduler_mode (void);

/*********************************
 * "Protected" scheduler methods shared by the scheduler implementations.
 * Only scheduler implementation code should call these.
 *********************************/

/* Re-entrant lock/unlock. */
void scheduler__lock (void);
void scheduler__unlock (void);

thread_type_t scheduler__get_thread_type (void);

/********************************
 * Each scheduler implementation must define these APIs.
 ********************************/

struct schedulerImpl_s;
typedef struct schedulerImpl_s schedulerImpl_t;

/* Initialize the scheduler implementation.
 * INPUTS:    mode: The mode in which the scheduler will run.
 *            args: Define this in your header file so users can parameterize you.
 * OUTPUTS:   schedulerImpl: Set the function pointers for the elements of your implementation.
 */
typedef void (*schedulerImpl_init) (scheduler_mode_t mode, void *args, schedulerImpl_t *schedulerImpl);

/* See scheduler_register_lcbn. */
typedef void (*schedulerImpl_register_lcbn) (lcbn_t *lcbn);
/* See scheduler_next_lcbn_type. */
typedef enum callback_type (*schedulerImpl_next_lcbn_type) (void);
/* See scheduler_thread_yield. */
typedef void (*schedulerImpl_thread_yield) (schedule_point_t point, void *schedule_point_details);
/* See scheduler_emit. */
typedef void (*schedulerImpl_emit) (char *output_file);
/* See scheduler_lcbns_remaining. */
typedef int  (*schedulerImpl_lcbns_remaining) (void);
/* See scheduler_schedule_has_diverged. */
typedef int  (*schedulerImpl_schedule_has_diverged) (void);

struct schedulerImpl_s
{
  schedulerImpl_register_lcbn register_lcbn;
  schedulerImpl_next_lcbn_type next_lcbn_type;
  schedulerImpl_thread_yield thread_yield;
  schedulerImpl_emit emit;
  schedulerImpl_lcbns_remaining lcbns_remaining;
  schedulerImpl_schedule_has_diverged schedule_has_diverged;
};

#if 0

/* The Logical CallBack Nodes the scheduler works with. */
struct sched_lcbn_s
{
  int magic;
  lcbn_t *lcbn;

  struct list_elem elem; /* TODO Can the user put a sched_lcbn_t in his own lists? Also, ideally this type would be opaque. */
};
typedef struct sched_lcbn_s sched_lcbn_t;

sched_lcbn_t *sched_lcbn_create (lcbn_t *lcbn);
void sched_lcbn_destroy (sched_lcbn_t *sched_lcbn);
void sched_lcbn_list_destroy_func (struct list_elem *e, void *aux);

/* Returns the next scheduled LCBN.
 * If nothing left to schedule, returns NULL.
 */
const lcbn_t * scheduler_next_scheduled_lcbn (void);

/* Replay: Construct lists of "ready contexts" for the scheduler (those which have a user callback ready to invoke). */

/* Where is the sched_context coming from? The list of possibly-invoked CBs varies based on the location. */
enum execution_context
{
  EXEC_CONTEXT_UV__RUN_TIMERS,
  EXEC_CONTEXT_UV__RUN_PENDING,
  EXEC_CONTEXT_UV__RUN_IDLE,
  EXEC_CONTEXT_UV__RUN_PREPARE,
  EXEC_CONTEXT_UV__IO_POLL,
  EXEC_CONTEXT_UV__RUN_CHECK,
  EXEC_CONTEXT_UV__RUN_CLOSING_HANDLES,

  EXEC_CONTEXT_THREADPOOL_WORKER,
  EXEC_CONTEXT_THREADPOOL_DONE
};

struct sched_context_s
{
  int magic;

  enum execution_context exec_context;
  enum callback_context cb_context;
  void *wrapper; /* uv_handle_t, uv_req_t?, uv_loop_t, struct uv__async */

  struct list_elem elem;
};
typedef struct sched_context_s sched_context_t;

sched_context_t *sched_context_create (enum execution_context exec_context, enum callback_context cb_context, void *wrapper);
void sched_context_destroy (sched_context_t *sched_context);
void sched_context_list_destroy_func (struct list_elem *e, void *aux);

/* Record. */

/* TODO The caller sets the global_exec_id for the LCBNs in invoke_callback.
     This puts the burden of tracking exec IDs on the caller instead of on the scheduler,
     which seems a bit odd. 
   Anyway, make sure you set lcbn->global_exec_id under a mutex! */


/* Replay. */

/* Determine the next context to invoke. 
   Input is a list of sched_context_t's. 
   REPLAY: Returns NULL if none of the specified contexts has the next LCBN in the schedule.

   TODO If there are none available in SCHED_CONTEXT (because running the context executes no CBs),
    we need to somehow indicate that you should run it to ensure forward progress. */
sched_context_t * scheduler_next_context (struct list *sched_context_list);

/* Determine the next LCBN to invoke from those available in SCHED_CONTEXT. 
   (internal only) Returns SILENT_CONTEXT if SCHED_CONTEXT has no ready LCBNs, i.e. if
     no user CBs will be invoked if we schedule it. This is a clue to schedule it.
   If none of those available in SCHED_CONTEXT is up next, returns NULL. 
   This should not happen if you provide the sched_context most recently returned by scheduler_next_context. 

   This API is relevant as used in scheduler_next_context, but not outside of
   the scheduler internals. For the majority of use cases, invoking
   a handle will inevitably result in invoking a series/stream/cluster/sequence of related LCBNs.
   For example, invoking uv__stream_io on a handle's uv__io_t may invoke an arbitrary number
   of LCBNs, and in a specific order.
   At the moment I do not wish to violate this order, so we'll see how trustworthy the order is
   under schedule variations. My hypothesis is that, provided you acknowledge fixed sequences
   when manipulating a schedule (e.g. not inserting another LCBN in the middle of a sequence), 
   the sequences will naturally occur in the recorded order. 
    
   Call sched_lcbn_is_next in invoke_callback to confirm or reject this hypothesis. */
sched_lcbn_t * scheduler_next_lcbn (sched_context_t *sched_context);


/* Block until SCHED_LCBN is next up.
   This allows competing threads to finish whatever they are doing.
   This is necessary if you call scheduler_advance prior to actually
   invoking a callback. */
void scheduler_block_until_next (sched_lcbn_t *sched_lcbn);

/* (scheduler_next_context) check if SCHED_LCBN is next on the schedule, or 
   (invoke_callback) verify that SCHED_LCBN is supposed to be next on the schedule. 
   
   REPLAY mode: If we go long enough without scheduler_advance'ing, calls to this function 
                may trigger a switch to RECORD mode (presuming a more subtle schedule 
                divergence than scheduler_advance detects).
                You can call with NULL to check for timeout. */
int sched_lcbn_is_next (sched_lcbn_t *sched_lcbn);

/* Tell the scheduler that the most-recent LCBN has been executed. 
   This can be done prior to executing an LCBN provided that the executing
   LCBN is allowed to complete before a new (non-nested) LCBN is invoked. 
   
   RECORD mode: Does some bookkeeping.
   REPLAY mode: Does bookeeping, checks for divergence, etc. */
void scheduler_advance (void);

/* For REPLAY mode.
   LCBN is a just-finished node. Check if it has diverged from the schedule.

   Divergence: The schedule has diverged if the children of LCBN are not exactly 
      (number, order, and type) as indicated in the input schedule.

   If divergence is detected, we can no longer REPLAY the input schedule because
     we are no longer seeing the input schedule.
   We respond by "diverging" (switching back into RECORD mode if acceptable based on min_n_executed_before_divergence_allowed.

   A divergent schedule can occur in one of two ways:
    - REPLAYing a RECORDed application, encountering non-determinism in some fashion
      e.g. branches that rely on wall clock time, random numbers, change in inputs.
    - REPLAYing a rescheduled application -- we hoped the schedule would remain the
      same after changing the order of observed events, but it didn't.

   Returns the schedule mode in place at the end of the function. 
   Test that or scheduler_has_diverged() for divergence.
*/
scheduler_mode_t scheduler_check_lcbn_for_divergence (lcbn_t *lcbn);

/* For REPLAY mode.
   cbt is the callback type of the next marker node, which we are trying to emit.
   Check against the schedule to see if we've diverged.

   Divergence example: We might have entered the loop and be presenting MARKER_RUN_TIMERS_1_BEGIN 
     instead of the expected MARKER_UV_RUN_END. I can't think of another case that wouldn't
     have been caught by the divergence timeout code instead.

   Same idea as scheduler_check_lcbn_for_divergence.
   Test the returned scheduler_mode_t or scheduler_has_diverged() for divergence.
*/
scheduler_mode_t scheduler_check_marker_for_divergence (enum callback_type cbt);

/* Each type of handle and req should declare a function of this type in internal.h
   for use in scheduler_next_context and scheduler_next_lcbn. 
     Name it like: uv__ready_*_lcbns {for * in async, check, fs_event, etc.} 
   It should return the list of sched_lcbn_t's that are available on the provided wrapper. */
typedef struct list * (*ready_lcbns_func)(void *wrapper, enum execution_context context);

void scheduler_UT (void);

#endif

#endif  /* UV_SRC_SCHEDULER_H_ */
