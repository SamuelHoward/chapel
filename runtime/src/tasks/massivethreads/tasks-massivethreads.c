/*
 * Copyright 2004-2016 Cray Inc.
 * Other additional copyright holders may be indicated within.
 * 
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 * 
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE

#include "chplrt.h"
#include "chplcast.h"
#include "chplcgfns.h" // for chpl_ftable
#include "chpl-comm.h"
#include "chpl-locale-model.h"
#include "chpl-mem.h"
#include "chplsys.h"
#include "chpl-tasks.h"
#include "error.h"
#include <assert.h>
#include <stdint.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sched.h>
#include <stdio.h>
#include <dlfcn.h>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <math.h>

#include "myth.h"

// When a task (=user-level thread) migrates with acquiring a
// pthread_mutex lock, the following pthread_mutex_unlock sometimes
// does not work correctly.
// For a workaround, MassiveThreads tasking layer suppresses task migrations
// when defined the following macro.
#define PTHREAD_MUTEX_OVERRIDE

typedef struct {
  // A task which acquires pthread_mutex_lock must not migrate to
  // another worker thread.
  int in_mutex_flag;
} thread_local_data;

static int tasking_layer_active = 0;
static int worker_in_cs_beforeinit = 0;
static thread_local_data* s_tld;

// task-private data of comm task
static const chpl_task_bundle_t s_def_chpl_data=
             { .requestedSubloc = c_sublocid_any_val };

// task-private data of main task
static chpl_task_bundle_t s_main_chpl_data =
             { .requestedSubloc = c_sublocid_any_val };

static inline chpl_task_bundle_t* getTaskPrivateData(void) {
  if (tasking_layer_active){
    // Why is this check here?
    if (myth_wsapi_get_hint_size(myth_self()) >= sizeof(chpl_task_bundle_t)){
      return (chpl_task_bundle_t*)myth_wsapi_get_hint_ptr(myth_self());
    }
  }
  return &s_main_chpl_data;
}

#ifdef PTHREAD_MUTEX_OVERRIDE

static int is_worker_in_cs(void) {
  if (tasking_layer_active) {
    int rank = myth_get_worker_num();
    assert(s_tld[rank].in_mutex_flag>=0);
    return s_tld[rank].in_mutex_flag>0;
  } else{
    assert(worker_in_cs_beforeinit>=0);
    return worker_in_cs_beforeinit>0;
  }
}

// #define CS_WARNING

static void worker_enter_cs(void) {
  if (tasking_layer_active) {
    int rank = myth_get_worker_num();
    s_tld[rank].in_mutex_flag++;
#ifdef CS_WARNING
    if (s_tld[rank].in_mutex_flag>1){
      fprintf(stderr,"warning:duplicated enter to critical section worker: %d\n",rank);
    }
#endif
  } else {
    worker_in_cs_beforeinit++;
#ifdef CS_WARNING
    if (worker_in_cs_beforeinit<0){
      fprintf(stderr,"warning:duplicated enter to critical section \n");
    }
#endif
  }
}

static void worker_exit_cs(void) {
  if (tasking_layer_active) {
    int rank = myth_get_worker_num();
    s_tld[rank].in_mutex_flag--;
    if (s_tld[rank].in_mutex_flag<0){
#ifdef CS_WARNING
      fprintf(stderr,"warning:exit from empty critical section worker: %d\n",rank);
#endif
      s_tld[rank].in_mutex_flag=0;
    }
  } else {
    worker_in_cs_beforeinit--;
    if (worker_in_cs_beforeinit<0){
#ifdef CS_WARNING
      fprintf(stderr,"warning:exit from empty critical section \n");
#endif
      worker_in_cs_beforeinit=0;
    }
  }
}

#include <pthread.h>

static int (*pthread_mutex_lock_fp)(pthread_mutex_t *) = NULL;
static int (*pthread_mutex_trylock_fp)(pthread_mutex_t *) = NULL;
static int (*pthread_mutex_unlock_fp)(pthread_mutex_t *) = NULL;

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  worker_enter_cs();
  if (!pthread_mutex_lock_fp) {
    pthread_mutex_lock_fp = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    assert(pthread_mutex_lock_fp);
  }
  pthread_mutex_lock_fp(mutex);
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  int ret;
  if (!pthread_mutex_trylock_fp) {
    pthread_mutex_trylock_fp = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    assert(pthread_mutex_lock_fp);
  }
  ret = pthread_mutex_trylock_fp(mutex);
  if (ret==0) worker_enter_cs();
  return ret;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (!pthread_mutex_unlock_fp) {
    pthread_mutex_unlock_fp = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    assert(pthread_mutex_unlock_fp);
  }
  pthread_mutex_unlock_fp(mutex);
  worker_exit_cs();
  return 0;
}


#else

static int is_worker_in_cs(void) {
  return 0;
}

static void worker_enter_cs(void) {
}

static void worker_exit_cs(void) {
}

#endif

// Sync variables
void chpl_sync_lock(chpl_sync_aux_t *s) {
  //Simple mutex lock
  assert(!is_worker_in_cs());
  {
    myth_felock_lock(s->lock);
  }
}
void chpl_sync_unlock(chpl_sync_aux_t *s) {
  assert(!is_worker_in_cs());
  //Simple mutex unlock
  myth_felock_unlock(s->lock);
}

void chpl_sync_waitFullAndLock(chpl_sync_aux_t *s, int32_t lineno,
    int32_t filename) {
  assert(!is_worker_in_cs());
  {
    //wait until F/E bit is empty, and acquire lock
    myth_felock_wait_lock(s->lock, 1);
  }
}

void chpl_sync_waitEmptyAndLock(chpl_sync_aux_t *s, int32_t lineno,
    int32_t filename) {
  assert(!is_worker_in_cs());
  {
    myth_felock_wait_lock(s->lock, 0);
  }
}

void chpl_sync_markAndSignalFull(chpl_sync_aux_t *s) {
  //release lock and set F/E bit to full
  myth_felock_set_unlock(s->lock, 1);
}

void chpl_sync_markAndSignalEmpty(chpl_sync_aux_t *s) {
  //release lock and set F/E bit to empty
  myth_felock_set_unlock(s->lock, 0);
}

chpl_bool chpl_sync_isFull(void *val_ptr, chpl_sync_aux_t *s) {
  //return whether F/E bit is full or not
  return myth_felock_status(s->lock);
}

void chpl_sync_initAux(chpl_sync_aux_t *s) {
  //init sync variable
  s->lock = myth_felock_create();
}

void chpl_sync_destroyAux(chpl_sync_aux_t *s) {
  //destroy sync variable
  myth_felock_destroy(s->lock);
}

static cpu_set_t worker_cpusets[CPU_SETSIZE];
static int available_cores = -1;

static void get_process_affinity_info(void) {
  cpu_set_t cset;
  int i;
  sched_getaffinity(getpid(), sizeof(cpu_set_t), &cset);
  for (i = 0; i < CPU_SETSIZE; i++) {
    CPU_ZERO(&worker_cpusets[i]);
  }
  available_cores = 0;
  for (i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &cset)) {
      CPU_SET(i, &worker_cpusets[available_cores]);
      available_cores++;
    }
  }
}

//Return the number of CPU cores
static int get_cpu_num(void) {
  assert(available_cores > 0);
  return available_cores;
}

static int32_t s_num_workers;
static size_t s_stack_size;

static volatile chpl_bool canCountRunningTasks = false;

static void* task_wrapper(void* a) {
  chpl_task_bundle_t* bundle = getTaskPrivateData();

  if (bundle->countRunning)
    chpl_taskRunningCntInc(0, 0);
  (bundle->requested_fn)(bundle);
  if (bundle->countRunning)
    chpl_taskRunningCntDec(0, 0);
  return NULL;
}

// Tasks
void chpl_task_init(void) {
  //Initialize tasking layer
  //initializing change the number of workers
  int32_t numThreadsPerLocale;
  int numCommTasks = chpl_comm_numPollingTasks();
  char *env;
  int i;
  void *chpl_data_ptr=&s_main_chpl_data;
  size_t chpl_data_size=sizeof(chpl_task_bundle_t);

  //
  // This threading layer does not have any inherent limit on the number
  // of threads.  Its limit is the lesser of any limits imposed by the
  // comm layer and the user.
  //
  uint32_t lim;
  // Exit from MassiveThreads, if activated.
  myth_fini();
  // get process affinity mask
  get_process_affinity_info();
  // Default, number of CPU cores
  numThreadsPerLocale = get_cpu_num();
  // override by MYTH_WORKER_NUM
  env = getenv("MYTH_WORKER_NUM");
  if (env) {
    if ((lim = atoi(env)) > 0)
      numThreadsPerLocale = lim;
  }
  // override by CHPL_RT_NUM_THREADS_PER_LOCALE
  if ((lim=chpl_task_getenvNumThreadsPerLocale())>0)
    numThreadsPerLocale=lim;
  // limit by comm layer limit
  if ((lim=chpl_comm_getMaxThreads())>0){
    numThreadsPerLocale=(numThreadsPerLocale > lim)?lim:numThreadsPerLocale;
  }

  s_num_workers = numThreadsPerLocale;
  if ((s_stack_size=chpl_task_getEnvCallStackSize())==0)
    s_stack_size=chpl_task_getDefaultCallStackSize();
  assert(s_stack_size > 0);
  assert(!is_worker_in_cs());
  s_tld = chpl_mem_allocMany(numThreadsPerLocale + numCommTasks,
      sizeof(thread_local_data), 0, 0, 0);
  for (i = 0; i < numThreadsPerLocale + numCommTasks; i++) {
    s_tld[i].in_mutex_flag = 0;
  }
  tasking_layer_active = 1;
  myth_init_withparam((int) (numThreadsPerLocale + numCommTasks), s_stack_size);
  // Assign task-private data to this main task
  s_main_chpl_data = s_def_chpl_data;
  myth_wsapi_set_hint(myth_self(),&chpl_data_ptr,&chpl_data_size);
}

int chpl_task_createCommTask(chpl_fn_p fn, void* arg) {
  myth_thread_option opt;
  myth_thread_t th;
  //chpl_fn_p is defined as "typedef void (*chpl_fn_p)(void*);" in chpltypes.h at line 85.
  //Since return value is always ignored, this cast is legal unless the definition is changed.
  opt.stack_size = 0;
  opt.switch_immediately = 0;
  opt.custom_data_size = sizeof(chpl_task_bundle_t);
  opt.custom_data = (void*)&s_def_chpl_data;
  th = myth_create_ex((void*(*)(void*)) fn, NULL, &opt);
  assert(th);
  myth_detach(th);
  return 0;
}

void chpl_task_exit(void) {
  //Cleanup tasking layer
  assert(!is_worker_in_cs());
  myth_fini();
  tasking_layer_active = 0;
  chpl_mem_free(s_tld, 0, 0);
}

void chpl_task_callMain(void(*chpl_main)(void)) {
  //Call main function
  chpl_main();
}

void chpl_task_stdModulesInitialized(void) {
  // It's not safe to call the module code to count the main task as
  // running until after the modules have been initialized.  That's
  // when this function is called, so now count the main task.
  canCountRunningTasks = true;
  chpl_taskRunningCntInc(0, 0);
}

void chpl_task_addToTaskList(chpl_fn_int_t fid,
    chpl_task_bundle_t* arg, size_t arg_size,
    c_sublocid_t subLoc,
    void **task_list, int32_t task_list_locale,
    chpl_bool is_begin_stmt, int lineno, int32_t filename) {
  //Create a new task directly
  myth_thread_option opt;
  myth_thread_t th;
  chpl_bool serial_state = getTaskPrivateData()->serial_state;
  chpl_task_prvData_t prv;

  memset(&prv, 0, sizeof(prv));

  if (serial_state) {
    (*chpl_ftable[fid])(arg);
    return;
  }

  arg->serial_state = serial_state;
  arg->countRunning = false;
  arg->is_executeOn = false;
  arg->requestedSubloc = subLoc;
  arg->requested_fn = chpl_ftable[fid];
  arg->prv = prv;

  //Create one task
  opt.stack_size = 0;
  opt.switch_immediately = (is_worker_in_cs())?0:1;
  // By storing the arguments in the custom_data pointer,
  // ask massivethreads to copy the data to the task stack.
  // Instead of using the task argument, task_wrapper will get
  // this task-local data and use it.
  opt.custom_data_size = arg_size;
  opt.custom_data = arg;
  th = myth_create_ex(task_wrapper, NULL, &opt);
  assert(th);
  myth_detach(th);
}

void chpl_task_executeTasksInList(void** task_list) {
  //Nothing to do because chpl_task_list is actually not used.
}

static inline
void taskCallBody(chpl_fn_p fp,
                  chpl_task_bundle_t* arg,
                  size_t arg_size,
                  c_sublocid_t subloc,
                  chpl_bool serial_state,
                  int lineno, int32_t filename)
{
  myth_thread_t th;
  myth_thread_option opt;
  chpl_task_prvData_t prv;

  memset(&prv, 0, sizeof(prv));

  arg->serial_state = serial_state;
  arg->countRunning = canCountRunningTasks;
  arg->is_executeOn = true;
  arg->requestedSubloc = subloc;
  arg->requested_fn = fp;
  arg->prv = prv;

  assert(subloc == 0 || subloc == c_sublocid_any);

  // Do the same as chpl_task_addToTaskList, except wrap the function
  opt.stack_size = 0;
  opt.switch_immediately = (is_worker_in_cs())?0:1;
  opt.custom_data_size = arg_size;
  opt.custom_data = arg;
  th = myth_create_ex(task_wrapper, NULL, &opt);
  assert(th);
  myth_detach(th);
}

void chpl_task_taskCall(chpl_fn_p fp,
                        chpl_task_bundle_t* arg,
                        size_t arg_size,
                        c_sublocid_t subloc,
                        int lineno,
                        int32_t fileno)
{
  taskCallBody(fp, arg, arg_size, subloc, false, lineno, fileno);
}


void chpl_task_startMovedTask(chpl_fn_p fp,
                              chpl_task_bundle_t* arg,
                              size_t arg_size,
                              c_sublocid_t subloc,
                              chpl_taskID_t id,
                              chpl_bool serial_state)
{
  assert(id == chpl_nullTaskID);
  taskCallBody(fp, arg, arg_size, subloc, serial_state, 0, 0);
}


c_sublocid_t chpl_task_getSubloc(void) {
  return 0;
}

void chpl_task_setSubloc(c_sublocid_t subloc) {
  assert(subloc == 0 || subloc == c_sublocid_any);
  getTaskPrivateData()->requestedSubloc=subloc;
}

c_sublocid_t chpl_task_getRequestedSubloc(void) {
  return getTaskPrivateData()->requestedSubloc;
}

chpl_taskID_t chpl_task_getId(void) {
  //get task private ID
  return (chpl_taskID_t) myth_self();
}

void chpl_task_yield(void) {
  //yield execution to other tasks
  myth_yield(1);
}


void chpl_task_sleep(double secs) {
  //sleep specified seconds
  struct timespec delay;
  delay.tv_sec = (time_t)(secs);
  delay.tv_nsec = (long)(1e9*(secs - floor(secs)));
  nanosleep(&delay, NULL);
}

chpl_bool chpl_task_getSerial(void)
{
  //get dynamic serial state
  return getTaskPrivateData()->serial_state;
}

void chpl_task_setSerial(chpl_bool new_state) {
  //set dynamic serial state
  getTaskPrivateData()->serial_state = new_state;
}

uint32_t chpl_task_getMaxPar(void) {
  uint32_t max;

  //
  // We expect that even if the physical CPU have multiple hardware
  // threads, cache and pipeline conflicts will typically prevent
  // applications from gaining by using them.  So, we just return the
  // lesser of the number of physical CPUs and the number of workers
  // we have.
  //
  max = (uint32_t) chpl_getNumPhysicalCpus(true);
  if ((uint32_t) s_num_workers < max)
    max = (uint32_t) s_num_workers;
  return max;
}

c_sublocid_t chpl_task_getNumSublocales(void)
{
  return 0;
}

chpl_task_prvData_t* chpl_task_getPrvData(void) {
  return & getTaskPrivateData()->prv;
}

size_t chpl_task_getCallStackSize(void) {
  //return call stack size
  return s_stack_size;
}

uint32_t chpl_task_getNumQueuedTasks(void) {
  //return the number of queued tasks
  return 0;
}

uint32_t chpl_task_getNumRunningTasks(void) {
  //return the number of running tasks
  chpl_internal_error("chpl_task_getNumRunningTasks() called");
  return 1;
}

int32_t chpl_task_getNumBlockedTasks(void) {
  //return the number of blocked tasks
  return 0;
}

uint32_t chpl_task_getNumThreads(void) {
  //return the number of threads (excluding a thread for comm)
  return s_num_workers;
}

uint32_t chpl_task_getNumIdleThreads(void) {
  //return the number of idle threads
  return 0;
}
