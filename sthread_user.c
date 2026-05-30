/* Simplethreads Instructional Thread Package
 * 
 * sthread_user.c - Implements the sthread API using user-level threads.
 *
 *    You need to implement the routines in this file.
 *
 * Change Log:
 * 2002-04-15        rick
 *   - Initial version.
 * 2005-10-12        jccc
 *   - Added semaphores, deleted conditional variables
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <sthread.h>
#include <sthread_user.h>
#include <sthread_ctx.h>
#include <sthread_time_slice.h>
#include <sthread_user.h>
#include "queue.h"

#define CLOCK_TICK      100
#define NUM_PRIO        15
#define STATIC_PRIO_MAX  4
#define DYN_PRIO_MIN     5
#define DYN_PRIO_MAX    14
#define QUANTUM_BASE     5

struct _sthread {
  sthread_ctx_t *saved_ctx;
  sthread_start_func_t start_routine_ptr;
  long wake_time;
  int join_tid;
  void* join_ret;
  void* args;
  int tid;

  /* escalonador */
  int priority;       /* prioridade actual (0..14)        */
  int base_priority;  /* prioridade atribuída na criação  */
  int quantum;        /* quantum ainda disponível         */
  int nice;           /* valor nice (0..10)               */
};
/* estrutura da runqueue */
typedef struct {
    queue_t *filas[NUM_PRIO];
    int      bitmap[NUM_PRIO];
    int      count;
} runqueue_t;

static runqueue_t  rq_storage[2];
static runqueue_t *rq_active;
static runqueue_t *rq_expired;

static queue_t *dead_thr_list;
static queue_t *sleep_thr_list;
static queue_t *join_thr_list;
static queue_t *zombie_thr_list;
static struct _sthread *active_thr;
static int tid_gen;


static queue_t *blocked_list;
static long Clock;

/*********************************************************************/
/* Part 1: Creating and Scheduling Threads                           */
/*********************************************************************/

/* inicializa uma runqueue */
static void rq_init(runqueue_t *rq)
{
    int i;
    for (i = 0; i < NUM_PRIO; i++) {
        rq->filas[i]  = create_queue();
        rq->bitmap[i] = 0;
    }
    rq->count = 0;
}

/* insere thread na fila correspondente à sua prioridade */
static void rq_insert(runqueue_t *rq, struct _sthread *thr)
{
    int p = thr->priority;
    queue_insert(rq->filas[p], thr);
    rq->bitmap[p] = 1;
    rq->count++;
}

/* remove e devolve a thread de maior prioridade (menor valor numérico) */
static struct _sthread *rq_remove_highest(runqueue_t *rq)
{
    int i;
    if (rq->count == 0) return NULL;
    for (i = 0; i < NUM_PRIO; i++) {
        if (rq->bitmap[i]) {
            struct _sthread *thr = queue_remove(rq->filas[i]);
            if (queue_is_empty(rq->filas[i]))
                rq->bitmap[i] = 0;
            rq->count--;
            return thr;
        }
    }
    return NULL;
}

/* troca runqueues quando as activas ficam vazias */
static void maybe_swap_runqueues(void)
{
    if (rq_active->count == 0) {
        runqueue_t *tmp = rq_active;
        rq_active  = rq_expired;
        rq_expired = tmp;
    }
}
void sthread_user_free(struct _sthread *thread);

void sthread_aux_start(void){
  splx(LOW);
  active_thr->start_routine_ptr(active_thr->args);
  sthread_user_exit((void*)0);
}

void sthread_user_dispatcher(void);

void sthread_user_init(void) {

  rq_init(&rq_storage[0]);
  rq_init(&rq_storage[1]);
  rq_active  = &rq_storage[0];
  rq_expired = &rq_storage[1];

  dead_thr_list   = create_queue();
  sleep_thr_list  = create_queue();
  join_thr_list   = create_queue();
  zombie_thr_list = create_queue();
  tid_gen = 1;
  blocked_list = create_queue();
  struct _sthread *main_thread = malloc(sizeof(struct _sthread));
  main_thread->start_routine_ptr = NULL;
  main_thread->args              = NULL;
  main_thread->saved_ctx         = sthread_new_blank_ctx();
  main_thread->wake_time         = 0;
  main_thread->join_tid          = 0;
  main_thread->join_ret          = NULL;
  main_thread->tid               = tid_gen++;
  main_thread->base_priority     = DYN_PRIO_MIN;
  main_thread->priority          = DYN_PRIO_MIN;
  main_thread->quantum           = QUANTUM_BASE;
  main_thread->nice              = 0;

  active_thr = main_thread;

  Clock = 1;
  sthread_time_slices_init(sthread_user_dispatcher, CLOCK_TICK);
}


sthread_t sthread_user_create(sthread_start_func_t start_routine, void *arg, int priority)
{
  return sthread_user_create_with_priority(start_routine, arg, priority);
}

sthread_t sthread_user_create_with_priority(sthread_start_func_t start_routine,
                                             void *arg, int priority)
{
  if (priority < 0)         priority = 0;
  if (priority >= NUM_PRIO) priority = NUM_PRIO - 1;

  struct _sthread *t = malloc(sizeof(struct _sthread));
  t->start_routine_ptr = start_routine;
  t->args              = arg;
  t->saved_ctx         = sthread_new_ctx(sthread_aux_start);
  t->wake_time         = 0;
  t->join_tid          = 0;
  t->join_ret          = NULL;
  t->base_priority     = priority;
  t->priority          = priority;
  t->quantum           = QUANTUM_BASE;
  t->nice              = 0;

  splx(HIGH);
  t->tid = tid_gen++;
  rq_insert(rq_active, t);
  splx(LOW);
  return t;
}


void sthread_user_exit(void *ret) {
  splx(HIGH);

  int is_zombie = 1;

  queue_t *tmp_queue = create_queue();
  while (!queue_is_empty(join_thr_list)) {
    struct _sthread *thread = queue_remove(join_thr_list);
    if (thread->join_tid == active_thr->tid) {
      thread->join_ret = ret;
      rq_insert(rq_active, thread);
      is_zombie = 0;
    } else {
      queue_insert(tmp_queue, thread);
    }
  }
  delete_queue(join_thr_list);
  join_thr_list = tmp_queue;

  if (is_zombie)
    queue_insert(zombie_thr_list, active_thr);
  else
    queue_insert(dead_thr_list, active_thr);

  maybe_swap_runqueues();
  if (rq_active->count == 0) {
    printf("Nenhuma thread executavel — a terminar.\n");
    exit(0);
  }

  struct _sthread *old_thr = active_thr;
  active_thr = rq_remove_highest(rq_active);
  sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);

  splx(LOW);
}
void sthread_user_dispatcher(void)
{
  Clock++;

  /* acorda threads em sleep */
  queue_t *tmp_queue = create_queue();
  while (!queue_is_empty(sleep_thr_list)) {
    struct _sthread *thread = queue_remove(sleep_thr_list);
    if (thread->wake_time <= Clock) {
      thread->wake_time = 0;
      rq_insert(rq_active, thread);
    } else {
      queue_insert(tmp_queue, thread);
    }
  }
  delete_queue(sleep_thr_list);
  sleep_thr_list = tmp_queue;

  /* decrementa quantum da thread activa */
  if (active_thr->quantum > 0)
    active_thr->quantum--;

  /* quantum esgotado — move para expiradas */
  if (active_thr->quantum == 0) {
    splx(HIGH);

    /* recalcula prioridade e quantum se dinâmica */
    if (active_thr->priority >= DYN_PRIO_MIN) {
      int nova_prio = active_thr->base_priority - active_thr->quantum + active_thr->nice;
      if (nova_prio < DYN_PRIO_MIN) nova_prio = DYN_PRIO_MIN;
      if (nova_prio > DYN_PRIO_MAX) nova_prio = DYN_PRIO_MAX;
      active_thr->priority = nova_prio;
      active_thr->quantum  = QUANTUM_BASE + 0 / 2;
    } else {
      active_thr->quantum = QUANTUM_BASE;
    }

    rq_insert(rq_expired, active_thr);
    maybe_swap_runqueues();

    struct _sthread *old_thr = active_thr;
    active_thr = rq_remove_highest(rq_active);
    sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
    splx(LOW);
  } else {
    sthread_user_yield();
  }
}


void sthread_user_yield(void)
{
  splx(HIGH);
  struct _sthread *old_thr = active_thr;
  rq_insert(rq_active, old_thr);
  maybe_swap_runqueues();
  active_thr = rq_remove_highest(rq_active);
  sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
  splx(LOW);
}




void sthread_user_free(struct _sthread *thread)
{
  sthread_free_ctx(thread->saved_ctx);
  free(thread);
}


/*********************************************************************/
/* Part 2: Join and Sleep Primitives                                 */
/*********************************************************************/

int sthread_user_join(sthread_t thread, void **value_ptr)
{
   /* suspends execution of the calling thread until the target thread
      terminates, unless the target thread has already terminated.
      On return from a successful pthread_join() call with a non-NULL 
      value_ptr argument, the value passed to pthread_exit() by the 
      terminating thread is made available in the location referenced 
      by value_ptr. When a pthread_join() returns successfully, the 
      target thread has been terminated. The results of multiple 
      simultaneous calls to pthread_join() specifying the same target 
      thread are undefined. If the thread calling pthread_join() is 
      canceled, then the target thread will not be detached. 

      If successful, the pthread_join() function returns zero. 
      Otherwise, an error number is returned to indicate the error. */

   
   splx(HIGH);
   // checks if the thread to wait is zombie
   int found = 0;
   queue_t *tmp_queue = create_queue();
   while (!queue_is_empty(zombie_thr_list)) {
      struct _sthread *zthread = queue_remove(zombie_thr_list);
      if (thread->tid == zthread->tid) {
         *value_ptr = thread->join_ret;
         queue_insert(dead_thr_list,thread);
         found = 1;
      } else {
         queue_insert(tmp_queue,zthread);
      }
   }
   delete_queue(zombie_thr_list);
   zombie_thr_list = tmp_queue;
  
   if (found) {
       splx(LOW);
       return 0;
   }

   
   // search active queue
   if (active_thr->tid == thread->tid) {
      found = 1;
   }
   
   queue_element_t *qe = NULL;

   // search exe
   qe = rq_active->filas[active_thr->priority]->first;
   while (!found && qe != NULL) {
      if (qe->thread->tid == thread->tid) {
         printf("Found in exe: tid=%d\n", thread->tid);
         found = 1;
      }
      qe = qe->next;
   }

   // search sleep
   qe = sleep_thr_list->first;
   while (!found && qe != NULL) {
      if (qe->thread->tid == thread->tid) {
         found = 1;
      }
      qe = qe->next;
   }

   // search join
   qe = join_thr_list->first;
   while (!found && qe != NULL) {
      if (qe->thread->tid == thread->tid) {
         found = 1;
      }
      qe = qe->next;
   }

   // if found blocks until thread ends
   if (!found) {
      splx(LOW);
      return -1;
   } else {
      active_thr->join_tid = thread->tid;
      
      struct _sthread *old_thr = active_thr;
      queue_insert(join_thr_list, old_thr);
      maybe_swap_runqueues();
      active_thr = rq_remove_highest(rq_active);
      printf ("Active is 0:%d\n", (active_thr == NULL));
      printf ("Old is 0:%d\n", (old_thr == NULL));
      sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
  
      *value_ptr = thread->join_ret;
   }
   
   splx(LOW);
   return 0;
}


int sthread_user_sleep(int time)
{
   splx(HIGH);
   
   long num_ticks = 10 * time / CLOCK_TICK;
   if (num_ticks == 0) {
      splx(LOW);
      
      return 0;
   }
   
   active_thr->wake_time = Clock + num_ticks;

   queue_insert(sleep_thr_list,active_thr); 
   sthread_t old_thr = active_thr;
   maybe_swap_runqueues();
active_thr = rq_remove_highest(rq_active);
   sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
   
   splx(LOW);
   return 0;
}

/* --------------------------------------------------------------------------*
 * Synchronization Primitives                                                *
 * ------------------------------------------------------------------------- */

/*
 * Mutex implementation
 */

struct _sthread_mutex
{
  lock_t l;
  struct _sthread *thr;
  queue_t* queue;
};

sthread_mutex_t sthread_user_mutex_init()
{
  sthread_mutex_t lock;

  if(!(lock = malloc(sizeof(struct _sthread_mutex)))){
    printf("Error in creating mutex\n");
    return 0;
  }

  /* mutex initialization */
  lock->l=0;
  lock->thr = NULL;
  lock->queue = create_queue();
  
  return lock;
}

void sthread_user_mutex_free(sthread_mutex_t lock)
{
  delete_queue(lock->queue);
  free(lock);
}

void sthread_user_mutex_lock(sthread_mutex_t lock)
{
  while(atomic_test_and_set(&(lock->l))) {}

  if(lock->thr == NULL){
    lock->thr = active_thr;

    atomic_clear(&(lock->l));
  } else {
    queue_insert(lock->queue, active_thr);
    
    atomic_clear(&(lock->l));

    splx(HIGH);
    struct _sthread *old_thr;
    old_thr = active_thr;
    //queue_insert(exe_thr_list, old_thr);
    maybe_swap_runqueues();
    active_thr = rq_remove_highest(rq_active);
    sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);

    splx(LOW);
  }
}

void sthread_user_mutex_unlock(sthread_mutex_t lock)
{
  if(lock->thr!=active_thr){
    printf("unlock without lock!\n");
    return;
  }

  while(atomic_test_and_set(&(lock->l))) {}

  if(queue_is_empty(lock->queue)){
    lock->thr = NULL;
  } else {
    lock->thr = queue_remove(lock->queue);
    rq_insert(rq_active, lock->thr);
  }

  atomic_clear(&(lock->l));
}

/*
 * Monitor implementation
 */

struct _sthread_mon {
 	sthread_mutex_t mutex;
	queue_t* queue;
};

sthread_mon_t sthread_user_monitor_init()
{
  sthread_mon_t mon;
  if(!(mon = malloc(sizeof(struct _sthread_mon)))){
    printf("Error creating monitor\n");
    return 0;
  }

  mon->mutex = sthread_user_mutex_init();
  mon->queue = create_queue();
  return mon;
}

void sthread_user_monitor_free(sthread_mon_t mon)
{
  sthread_user_mutex_free(mon->mutex);
  delete_queue(mon->queue);
  free(mon);
}

void sthread_user_monitor_enter(sthread_mon_t mon)
{
  sthread_user_mutex_lock(mon->mutex);
}

void sthread_user_monitor_exit(sthread_mon_t mon)
{
  sthread_user_mutex_unlock(mon->mutex);
}

void sthread_user_monitor_wait(sthread_mon_t mon)
{
  struct _sthread *temp;

  if(mon->mutex->thr != active_thr){
    printf("monitor wait called outside monitor\n");
    return;
  }

  /* inserts thread in queue of blocked threads */
  temp = active_thr;
  queue_insert(mon->queue, temp);
  queue_insert(blocked_list, temp);
  /* exits mutual exclusion region */
  sthread_user_mutex_unlock(mon->mutex);

	splx(HIGH);
	struct _sthread *old_thr;
	old_thr = active_thr;
	//queue_insert(exe_thr_list, old_thr);
	maybe_swap_runqueues();
        active_thr = rq_remove_highest(rq_active);
	sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
	splx(LOW);
}

void sthread_user_monitor_signal(sthread_mon_t mon)
{
  struct _sthread *temp;

  if(mon->mutex->thr != active_thr){
    printf("monitor signal called outside monitor\n");
    return;
  }

  while(atomic_test_and_set(&(mon->mutex->l))) {}
  if(!queue_is_empty(mon->queue)){
    /* changes blocking queue for thread */
    temp = queue_remove(mon->queue);
    queue_insert(mon->mutex->queue, temp);

    /* remove da blocked list global */
    queue_t *tmp = create_queue();
    while (!queue_is_empty(blocked_list)) {
      struct _sthread *t = queue_remove(blocked_list);
      if (t->tid != temp->tid)
        queue_insert(tmp, t);
    }
    delete_queue(blocked_list);
    blocked_list = tmp;
  }
  atomic_clear(&(mon->mutex->l));

}
/* The following functions are dummies to 
 * highlight the fact that pthreads do not
 * include monitors.
 */
int sthread_user_nice(int nice)
{
  if (nice < 0)  nice = 0;
  if (nice > 10) nice = 10;

  active_thr->nice = nice;

  /* calcula a prioridade que terá na próxima época */
  int nova_prio = active_thr->base_priority - active_thr->quantum + nice;
  if (nova_prio < DYN_PRIO_MIN) nova_prio = DYN_PRIO_MIN;
  if (nova_prio > DYN_PRIO_MAX) nova_prio = DYN_PRIO_MAX;

  return nova_prio;
}
sthread_mon_t sthread_dummy_monitor_init()
{
   printf("WARNING: pthreads do not include monitors!\n");
   return NULL;
}

void sthread_user_dump(void)
{
  int i;
  queue_element_t *qe;

  printf("=== dump start ===\n");

  /* thread activa */
  printf("active thread\n");
  printf("id: %d\n", active_thr->tid);
  printf("priority: %d\n", active_thr->priority);
  printf("quantum: %d\n", active_thr->quantum);

  /* runqueue activas */
  printf("active runqueue\n");
  for (i = 0; i < NUM_PRIO; i++) {
    printf("[%d]", i);
    qe = rq_active->filas[i]->first;
    while (qe != NULL) {
      printf(" %d,%d", qe->thread->tid, qe->thread->quantum);
      qe = qe->next;
    }
    printf("\n");
  }

  /* runqueue expiradas */
  printf("expired runqueue\n");
  for (i = 0; i < NUM_PRIO; i++) {
    printf("[%d]", i);
    qe = rq_expired->filas[i]->first;
    while (qe != NULL) {
      printf(" %d,%d", qe->thread->tid, qe->thread->quantum);
      qe = qe->next;
    }
    printf("\n");
  }

  /* lista de bloqueadas */
  printf("blocked list\n");
  qe = blocked_list->first;
  while (qe != NULL) {
    printf(" %d,%d", qe->thread->tid, qe->thread->quantum);
    qe = qe->next;
  }
  printf("\n");

  printf("=== dump end ===\n");
}
void sthread_dummy_monitor_free(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_enter(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_exit(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_wait(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_signal(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


