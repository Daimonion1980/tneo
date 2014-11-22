/*******************************************************************************
 *
 * TNeoKernel: real-time kernel initially based on TNKernel
 *
 *    TNKernel:                  copyright � 2004, 2013 Yuri Tiomkin.
 *    PIC32-specific routines:   copyright � 2013, 2014 Anders Montonen.
 *    TNeoKernel:                copyright � 2014       Dmitry Frank.
 *
 *    TNeoKernel was born as a thorough review and re-implementation of
 *    TNKernel. The new kernel has well-formed code, inherited bugs are fixed
 *    as well as new features being added, and it is tested carefully with
 *    unit-tests.
 *
 *    API is changed somewhat, so it's not 100% compatible with TNKernel,
 *    hence the new name: TNeoKernel.
 *
 *    Permission to use, copy, modify, and distribute this software in source
 *    and binary forms and its documentation for any purpose and without fee
 *    is hereby granted, provided that the above copyright notice appear
 *    in all copies and that both that copyright notice and this permission
 *    notice appear in supporting documentation.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE DMITRY FRANK AND CONTRIBUTORS "AS IS"
 *    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *    PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL DMITRY FRANK OR CONTRIBUTORS BE
 *    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *    THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

/**
 * \file
 *
 * Kernel system routines.
 *   
 */


/*******************************************************************************
 *    INCLUDED FILES
 ******************************************************************************/

#include "tn_common.h"
#include "tn_sys.h"

//-- internal tnkernel headers
#include "_tn_sys.h"
#include "_tn_timer.h"
#include "_tn_tasks.h"
#include "_tn_list.h"


#include "tn_tasks.h"
#include "tn_timer.h"


//-- for memcmp()
#include <string.h>

//-- self-check 
#if !defined(TN_PRIORITIES_MAX_CNT)
#  error TN_PRIORITIES_MAX_CNT is not defined
#endif

//-- check TN_PRIORITIES_CNT
#if (TN_PRIORITIES_CNT > TN_PRIORITIES_MAX_CNT)
#  error TN_PRIORITIES_CNT is too large (maximum is TN_PRIORITIES_MAX_CNT)
#endif


/*******************************************************************************
 *    PRIVATE TYPES
 ******************************************************************************/


/*******************************************************************************
 *    PUBLIC DATA
 ******************************************************************************/

/*
 * For comments on these variables, please see _tn_sys.h file.
 */
struct TN_ListItem tn_ready_list[TN_PRIORITIES_CNT];
struct TN_ListItem tn_create_queue;
volatile int tn_created_tasks_cnt;

unsigned short tn_tslice_ticks[TN_PRIORITIES_CNT];

volatile enum TN_StateFlag tn_sys_state;

struct TN_Task *tn_next_task_to_run;
struct TN_Task *tn_curr_run_task;

volatile unsigned int tn_ready_to_run_bmp;


#if !TN_DYNAMIC_TICK
volatile TN_SysTickCnt tn_sys_time_count;
#endif


#if TN_MUTEX_DEADLOCK_DETECT
volatile int tn_deadlocks_cnt = 0;
#endif

//-- System tasks

//-- idle task - priority (TN_PRIORITIES_CNT - 1) - lowest

struct TN_Task  tn_idle_task;
static void _idle_task_body(void * par);

/**
 * Pointer to user idle loop function
 */
TN_CBIdle        *tn_callback_idle_hook = TN_NULL;

/**
 * User-provided callback function that is called whenever 
 * event occurs (say, deadlock becomes active or inactive)
 */
TN_CBDeadlock    *tn_callback_deadlock = TN_NULL;

#if TN_DYNAMIC_TICK
TN_CBTickSchedule      *_tn_cb_tick_schedule = TN_NULL;
TN_CBTickCntGet        *_tn_cb_tick_cnt_get  = TN_NULL;
#endif



/*******************************************************************************
 *    PRIVATE DATA
 ******************************************************************************/

_TN_BUILD_CFG_DEFINE(static const, _build_cfg);



/*******************************************************************************
 *    EXTERNAL FUNCTION PROTOTYPES
 ******************************************************************************/

const struct _TN_BuildCfg *tn_app_build_cfg_get(void);



/*******************************************************************************
 *    PRIVATE FUNCTIONS
 ******************************************************************************/

/**
 * Idle task body. In fact, this task is always in RUNNABLE state.
 */
static void _idle_task_body(void *par)
{
   //-- enter endless loop with calling user-provided hook function
   for(;;)
   {
      tn_callback_idle_hook();
   }
}

/**
 * Manage round-robin (if used)
 */
#if TN_DYNAMIC_TICK

static _TN_INLINE void _round_robin_manage(void) {/*TODO*/}

#else

static _TN_INLINE void _round_robin_manage(void)
{
   //-- volatile is used here only to solve
   //   IAR(c) compiler's high optimization mode problem
   _TN_VOLATILE_WORKAROUND struct TN_ListItem *curr_que;
   _TN_VOLATILE_WORKAROUND struct TN_ListItem *pri_queue;
   _TN_VOLATILE_WORKAROUND int priority = tn_curr_run_task->priority;

   if (tn_tslice_ticks[priority] != TN_NO_TIME_SLICE){
      tn_curr_run_task->tslice_count++;

      if (tn_curr_run_task->tslice_count > tn_tslice_ticks[priority]){
         tn_curr_run_task->tslice_count = 0;

         pri_queue = &(tn_ready_list[priority]);
         //-- If ready queue is not empty and there are more than 1 
         //   task in the queue
         if (     !(_tn_list_is_empty((struct TN_ListItem *)pri_queue))
               && pri_queue->next->next != pri_queue
            )
         {
            //-- Remove task from head and add it to the tail of
            //-- ready queue for current priority

            curr_que = _tn_list_remove_head(&(tn_ready_list[priority]));
            _tn_list_add_tail(
                  &(tn_ready_list[priority]),
                  (struct TN_ListItem *)curr_que
                  );
         }
      }
   }
}

#endif


#if _TN_ON_CONTEXT_SWITCH_HANDLER
#if TN_PROFILER
/**
 * This function is called at every context switch, if `#TN_PROFILER` is 
 * non-zero.
 *
 * @param task_prev
 *    Task that was running, and now it is going to wait
 * @param task_new
 *    Task that was waiting, and now it is going to run
 */
static _TN_INLINE void _tn_sys_on_context_switch_profiler(
      struct TN_Task *task_prev,
      struct TN_Task *task_new
      )
{
#if TN_DEBUG
   //-- interrupts should be disabled here
   if (!TN_IS_INT_DISABLED()){
      _TN_FATAL_ERROR("");
   }
#endif
   TN_SysTickCnt cur_tick_cnt = _tn_sys_time_get();

   //-- handle task_prev (the one that was running and going to wait) {{{
   {
#if TN_DEBUG
      if (!task_prev->profiler.is_running){
         _TN_FATAL_ERROR();
      }
      task_prev->profiler.is_running = 0;
#endif

      //-- get difference between current time and last saved time:
      //   this is the time task was running.
      TN_SysTickCnt cur_run_time
         = (TN_SysTickCnt)(cur_tick_cnt - task_prev->profiler.last_tick_cnt);

      //-- add it to total run time
      task_prev->profiler.timing.total_run_time += cur_run_time;

      //-- check if we should update consecutive max run time
      if (task_prev->profiler.timing.max_consecutive_run_time < cur_run_time){
         task_prev->profiler.timing.max_consecutive_run_time = cur_run_time;
      }

      //-- update current task state
      task_prev->profiler.last_tick_cnt      = cur_tick_cnt;
      task_prev->profiler.last_wait_reason   = task_prev->task_wait_reason;
   }
   // }}}

   //-- handle task_new (the one that was waiting and going to run) {{{
   {
#if TN_DEBUG
      if (task_new->profiler.is_running){
         _TN_FATAL_ERROR();
      }
      task_new->profiler.is_running = 1;
#endif

      //-- get difference between current time and last saved time:
      //   this is the time task was waiting.
      TN_SysTickCnt cur_wait_time
         = (TN_SysTickCnt)(cur_tick_cnt - task_new->profiler.last_tick_cnt);

      //-- add it to total total_wait_time for particular wait reason
      task_new->profiler.timing.total_wait_time
         [ task_new->profiler.last_wait_reason ] 
         += cur_wait_time;

      //-- check if we should update consecutive max wait time
      if (
            task_new->profiler.timing.max_consecutive_wait_time
            [ task_new->profiler.last_wait_reason ] < cur_wait_time
         )
      {
         task_new->profiler.timing.max_consecutive_wait_time
            [ task_new->profiler.last_wait_reason ] = cur_wait_time;
      }

      //-- increment the counter of times task got running
      task_new->profiler.timing.got_running_cnt++;

      //-- update current task state
      task_new->profiler.last_tick_cnt      = cur_tick_cnt;
   }
   // }}}
}
#else

/**
 * Stub empty function, it is needed when `#TN_PROFILER` is zero.
 */
static _TN_INLINE void _tn_sys_on_context_switch_profiler(
      struct TN_Task *task_prev, //-- task was running, going to wait
      struct TN_Task *task_new   //-- task was waiting, going to run
      )
{}
#endif
#endif

/**
 * Create idle task, the task is NOT started after creation.
 */
static _TN_INLINE enum TN_RCode _idle_task_create(
      TN_UWord      *idle_task_stack,
      unsigned int   idle_task_stack_size
      )
{
   return tn_task_create(
         (struct TN_Task*)&tn_idle_task,  //-- task TCB
         _idle_task_body,                 //-- task function
         TN_PRIORITIES_CNT - 1,           //-- task priority
         idle_task_stack,                 //-- task stack
         idle_task_stack_size,            //-- task stack size
                                          //   (in int, not bytes)
         TN_NULL,                         //-- task function parameter
         (_TN_TASK_CREATE_OPT_IDLE)       //-- Creation option
         );
}

#if TN_CHECK_BUILD_CFG
static void _build_cfg_check(void)
{
   const struct _TN_BuildCfg *app_build_cfg = tn_app_build_cfg_get();
   if (_build_cfg.priorities_cnt != app_build_cfg->priorities_cnt){
      _TN_FATAL_ERROR("TN_PRIORITIES_CNT doesn't match");
   }

   if (_build_cfg.check_param != app_build_cfg->check_param){
      _TN_FATAL_ERROR("TN_CHECK_PARAM doesn't match");
   }

   if (_build_cfg.debug != app_build_cfg->debug){
      _TN_FATAL_ERROR("TN_DEBUG doesn't match");
   }

   if (_build_cfg.use_mutexes != app_build_cfg->use_mutexes){
      _TN_FATAL_ERROR("TN_USE_MUTEXES doesn't match");
   }

   if (_build_cfg.mutex_rec != app_build_cfg->mutex_rec){
      _TN_FATAL_ERROR("TN_MUTEX_REC doesn't match");
   }

   if (_build_cfg.mutex_deadlock_detect != app_build_cfg->mutex_deadlock_detect){
      _TN_FATAL_ERROR("TN_MUTEX_DEADLOCK_DETECT doesn't match");
   }

   if (_build_cfg.tick_lists_cnt_minus_one != app_build_cfg->tick_lists_cnt_minus_one){
      _TN_FATAL_ERROR("TN_TICK_LISTS_CNT doesn't match");
   }

   if (_build_cfg.api_make_alig_arg != app_build_cfg->api_make_alig_arg){
      _TN_FATAL_ERROR("TN_API_MAKE_ALIG_ARG doesn't match");
   }

   if (_build_cfg.profiler != app_build_cfg->profiler){
      _TN_FATAL_ERROR("TN_PROFILER doesn't match");
   }

   if (_build_cfg.dynamic_tick != app_build_cfg->dynamic_tick){
      _TN_FATAL_ERROR("TN_DYNAMIC_TICK doesn't match");
   }

#if defined (__TN_ARCH_PIC24_DSPIC__)
   if (_build_cfg.arch.p24.p24_sys_ipl != app_build_cfg->arch.p24.p24_sys_ipl){
      _TN_FATAL_ERROR("TN_P24_SYS_IPL doesn't match");
   }
#endif


   //-- for the case I forgot to add some param above, perform generic check
   TN_BOOL cfg_match = 
      !memcmp(&_build_cfg, app_build_cfg, sizeof(_build_cfg));

   if (!cfg_match){
      _TN_FATAL_ERROR("configuration mismatch");
   }
}
#else 

static inline void _build_cfg_check(void) {}

#endif




/*******************************************************************************
 *    PUBLIC FUNCTIONS
 ******************************************************************************/

/*
 * See comments in the header file (tn_sys.h)
 */
void tn_sys_start(
      TN_UWord            *idle_task_stack,
      unsigned int         idle_task_stack_size,
      TN_UWord            *int_stack,
      unsigned int         int_stack_size,
      TN_CBUserTaskCreate *cb_user_task_create,
      TN_CBIdle           *cb_idle
      )
{
   int i;
   enum TN_RCode rc;

#if TN_DYNAMIC_TICK
   //-- if dynamic tick is used, check that we have callbacks set.
   //   (they should be set by tn_callback_dyn_tick_set() before calling
   //   tn_sys_start())
   if (_tn_cb_tick_schedule == TN_NULL || _tn_cb_tick_cnt_get == TN_NULL){
      _TN_FATAL_ERROR("");
   }
#else
   //-- reset system time
   tn_sys_time_count = 0;
#endif

   //-- check that build configuration for the kernel and application matches
   //   (if only TN_CHECK_BUILD_CFG is non-zero)
   _build_cfg_check();

   //-- for each priority: 
   //   - reset list of runnable tasks with this priority
   //   - reset time slice to `#TN_NO_TIME_SLICE`
   for (i = 0; i < TN_PRIORITIES_CNT; i++){
      _tn_list_reset(&(tn_ready_list[i]));
      tn_tslice_ticks[i] = TN_NO_TIME_SLICE;
   }

   //-- reset generic task queue and task count to 0
   _tn_list_reset(&tn_create_queue);
   tn_created_tasks_cnt = 0;

   //-- initial system flags: no flags set (see enum TN_StateFlag)
   tn_sys_state = (enum TN_StateFlag)(0);  

   //-- reset bitmask of priorities with runnable tasks
   tn_ready_to_run_bmp = 0;

   //-- reset pointers to currently running task and next task to run
   tn_next_task_to_run = TN_NULL;
   tn_curr_run_task    = TN_NULL;

   //-- remember user-provided callbacks
   tn_callback_idle_hook = cb_idle;

   //-- Fill interrupt stack space with TN_FILL_STACK_VAL
   for (i = 0; i < int_stack_size; i++){
      int_stack[i] = TN_FILL_STACK_VAL;
   }

   //-- init timers
   _tn_timers_init();

   /*
    * NOTE: we need to separate creation of tasks and making them runnable,
    *       because otherwise tn_next_task_to_run would point on the task
    *       that isn't yet created, and it produces issues
    *       with order of task creation.
    *
    *       We should keep as little surprizes in the code as possible,
    *       so, it's better to just separate these steps and avoid any tricks.
    */

   //-- create system tasks
   rc = _idle_task_create(idle_task_stack, idle_task_stack_size);
   if (rc != TN_RC_OK){
      _TN_FATAL_ERROR("failed to create idle task");
   }

   //-- Just for the _tn_task_set_runnable() proper operation
   tn_next_task_to_run = &tn_idle_task; 

   //-- make system tasks runnable
   rc = _tn_task_activate(&tn_idle_task);
   if (rc != TN_RC_OK){
      _TN_FATAL_ERROR("failed to activate idle task");
   }

   //-- set tn_curr_run_task to idle task
   tn_curr_run_task = &tn_idle_task;
#if TN_PROFILER
#if TN_DEBUG
   tn_idle_task.profiler.is_running = 1;
#endif
#endif

   //-- now, we can create user's task(s)
   //   (by user-provided callback)
   cb_user_task_create();

   //-- set flag that system is running
   //   (well, it will be running soon actually)
   tn_sys_state |= TN_STATE_FLAG__SYS_RUNNING;

   //-- call architecture-dependent initialization and run the kernel:
   //   (perform first context switch)
   _tn_arch_sys_start(int_stack, int_stack_size);

   //-- should never be here
   _TN_FATAL_ERROR("should never be here");
}



/*
 * See comments in the header file (tn_sys.h)
 */
enum TN_RCode tn_tick_int_processing(void)
{
   enum TN_RCode rc = TN_RC_OK;

   if (!tn_is_isr_context()){
      rc = TN_RC_WCONTEXT;
   } else {
      TN_INTSAVE_DATA_INT;

      TN_INT_IDIS_SAVE();

#if !TN_DYNAMIC_TICK
      //-- increment system timer
      tn_sys_time_count++;
#endif

      //-- manage round-robin (if used)
      _round_robin_manage();

      //-- manage timers
      _tn_timers_tick_proceed();

      TN_INT_IRESTORE();
      _TN_CONTEXT_SWITCH_IPEND_IF_NEEDED();

   }
   return rc;
}

/*
 * See comments in the header file (tn_sys.h)
 */
enum TN_RCode tn_sys_tslice_set(int priority, int ticks)
{
   enum TN_RCode rc = TN_RC_OK;

   if (!tn_is_task_context()){
      rc = TN_RC_WCONTEXT;
   } else if (0
         || priority < 0 || priority >= (TN_PRIORITIES_CNT - 1)
         || ticks    < 0 || ticks    >   TN_MAX_TIME_SLICE)
   {
      rc = TN_RC_WPARAM;
   } else {
      TN_INTSAVE_DATA;

      TN_INT_DIS_SAVE();
      tn_tslice_ticks[priority] = ticks;
      TN_INT_RESTORE();
   }
   return rc;
}

/*
 * See comments in the header file (tn_sys.h)
 */
TN_SysTickCnt tn_sys_time_get(void)
{
   TN_SysTickCnt ret;
   TN_INTSAVE_DATA;

   TN_INT_DIS_SAVE();
   ret = _tn_sys_time_get();
   TN_INT_RESTORE();

   return ret;
}

/*
 * Returns current state flags (tn_sys_state)
 */
enum TN_StateFlag tn_sys_state_flags_get(void)
{
   return tn_sys_state;
}

/*
 * See comment in tn_sys.h file
 */
void tn_callback_deadlock_set(TN_CBDeadlock *cb)
{
   tn_callback_deadlock = cb;
}

/*
 * See comment in tn_sys.h file
 */
enum TN_Context tn_sys_context_get(void)
{
   enum TN_Context ret;

   if (tn_sys_state & TN_STATE_FLAG__SYS_RUNNING){
      ret = _tn_arch_inside_isr()
         ? TN_CONTEXT_ISR
         : TN_CONTEXT_TASK;
   } else {
      ret = TN_CONTEXT_NONE;
   }

   return ret;
}

/*
 * See comment in tn_sys.h file
 */
struct TN_Task *tn_cur_task_get(void)
{
   return tn_curr_run_task;
}

/*
 * See comment in tn_sys.h file
 */
TN_TaskBody *tn_cur_task_body_get(void)
{
   return tn_curr_run_task->task_func_addr;
}


#if TN_DYNAMIC_TICK

void tn_callback_dyn_tick_set(
      TN_CBTickSchedule   *cb_tick_schedule,
      TN_CBTickCntGet     *cb_tick_cnt_get
      )
{
   _tn_cb_tick_schedule = cb_tick_schedule;
   _tn_cb_tick_cnt_get  = cb_tick_cnt_get;
}

#endif




/*******************************************************************************
 *    PROTECTED FUNCTIONS
 ******************************************************************************/

/**
 * See comment in the _tn_sys.h file
 */
void _tn_wait_queue_notify_deleted(struct TN_ListItem *wait_queue)
{
   struct TN_Task *task;         //-- "cursor" for the loop iteration
   struct TN_Task *tmp_task;     //-- we need for temporary item because
                                 //   item is removed from the list
                                 //   in _tn_mutex_do_unlock().


   //-- iterate through all tasks in the wait_queue,
   //   calling _tn_task_wait_complete() for each task,
   //   and setting TN_RC_DELETED as a wait return code.
   _tn_list_for_each_entry_safe(
         task, struct TN_Task, tmp_task, wait_queue, task_queue 
         )
   {
      //-- call _tn_task_wait_complete for every task
      _tn_task_wait_complete(task, TN_RC_DELETED);
   }

#if TN_DEBUG
   if (!_tn_list_is_empty(wait_queue)){
      _TN_FATAL_ERROR("");
   }
#endif
}

/**
 * See comments in the file _tn_sys.h
 */
enum TN_StateFlag _tn_sys_state_flags_set(enum TN_StateFlag flags)
{
   enum TN_StateFlag ret = tn_sys_state;
   tn_sys_state |= flags;
   return ret;
}

/**
 * See comments in the file _tn_sys.h
 */
enum TN_StateFlag _tn_sys_state_flags_clear(enum TN_StateFlag flags)
{
   enum TN_StateFlag ret = tn_sys_state;
   tn_sys_state &= ~flags;
   return ret;
}


#if TN_MUTEX_DEADLOCK_DETECT
/**
 * See comments in the file _tn_sys.h
 */
void _tn_cry_deadlock(TN_BOOL active, struct TN_Mutex *mutex, struct TN_Task *task)
{
   if (active){
      //-- deadlock just became active
      if (tn_deadlocks_cnt == 0){
         _tn_sys_state_flags_set(TN_STATE_FLAG__DEADLOCK);
      }

      tn_deadlocks_cnt++;
   } else {
      //-- deadlock just became inactive
      tn_deadlocks_cnt--;

      if (tn_deadlocks_cnt == 0){
         _tn_sys_state_flags_clear(TN_STATE_FLAG__DEADLOCK);
      }
   }

   //-- if user has specified callback function for deadlock detection,
   //   notify him by calling this function
   if (tn_callback_deadlock != TN_NULL){
      tn_callback_deadlock(active, mutex, task);
   }

}
#endif

#if _TN_ON_CONTEXT_SWITCH_HANDLER
/*
 * See comments in the file _tn_sys.h
 */
void _tn_sys_on_context_switch(
      struct TN_Task *task_prev,
      struct TN_Task *task_new
      )
{
   _tn_sys_on_context_switch_profiler(task_prev, task_new);
   //TODO: stack overflow check
}
#endif


