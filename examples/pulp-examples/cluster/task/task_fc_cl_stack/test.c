/* 
 * Copyright (C) 2017 ETH Zurich, University of Bologna and GreenWaves Technologies
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license.  See the LICENSE file for details.
 *
 * Authors: Germain Haugou, ETH (germain.haugou@iis.ee.ethz.ch)
 */

#include "rt/rt_api.h"
#include "stdio.h"

#define MAX_NB_FC_TASKS 2
#define MAX_NB_CL_TASKS 16

static int nb_failures;
static volatile RT_L1_DATA int result[MAX_NB_FC_TASKS];
static RT_L1_DATA rt_task_t tasks[MAX_NB_FC_TASKS][MAX_NB_CL_TASKS];
static RT_L1_DATA void *stacks[MAX_NB_FC_TASKS][MAX_NB_CL_TASKS];
static RT_L1_DATA int stacks_size[MAX_NB_FC_TASKS][MAX_NB_CL_TASKS];
static RT_L1_DATA void *fc_stacks[MAX_NB_FC_TASKS];
static RT_L1_DATA int fc_stacks_size[MAX_NB_FC_TASKS];
static rt_task_t fc_tasks[MAX_NB_FC_TASKS];
static rt_event_t *events[MAX_NB_FC_TASKS];
static volatile int nb_fc_tasks_enqueued;
static volatile int nb_fc_tasks_to_enqueue;
static rt_task_cluster_t cluster;

static void end_of_fc_task(void *_task);

static void cl_task_entry(rt_task_t *task, int id)
{
  int task_id = task->args[0];
  rt_team_critical_enter();
  result[task_id] += (int)task + id;
  rt_team_critical_exit();

  volatile int i;
  for (i=0; i<id*10; i++);
}

static void fc_task_entry(rt_task_t *task, int id)
{
  int expected = 0;
  int task_id = task->args[0];
  int nb_tasks = task->args[1];
  result[task_id] = 0;

  for (int i=0; i<nb_tasks; i++)
  {
    rt_task_t *task = &tasks[task_id][i];
    task->args[0] = task_id;
    rt_task_init(task, cl_task_entry);
    rt_task_stacks(task, stacks[task_id][i], stacks_size[task_id][i]);
    int nb_cores = (i & (rt_nb_pe()-1)) + 1;
    expected += ((int)task)*nb_cores + (nb_cores-1)*nb_cores/2;
    rt_task_cores(task, nb_cores);
    rt_task_cl_push(task);
  }

  for (int i=0; i<nb_tasks; i++)
  {
    rt_task_t *task = &tasks[task_id][i];
    rt_task_cl_wait(task);
  }

  if (expected != result[task_id])
  {
    printf("ERROR, wrong checksum (expected: 0x%x, got: 0x%x)\n", expected, result[task_id]);
    rt_team_critical_enter();
    nb_failures++;
    rt_team_critical_exit();
  }
}

static void enqueue_fc_task(rt_task_t *task)
{
  int task_id = task->args[0];
  rt_task_init(task, fc_task_entry);
  rt_task_stacks(task, fc_stacks[task_id], fc_stacks_size[task_id]);
  nb_fc_tasks_to_enqueue--;
  nb_fc_tasks_enqueued++;
  rt_event_t *event = rt_event_get(NULL, end_of_fc_task, task);
  rt_task_fc_push(&cluster, task, event);
}

static void end_of_fc_task(void *_task)
{
  rt_task_t *task = (rt_task_t *)_task;
  nb_fc_tasks_enqueued--;

  while (nb_fc_tasks_to_enqueue > 0 && nb_fc_tasks_enqueued < MAX_NB_FC_TASKS)
  {
    enqueue_fc_task(task);
  }
}

static void check_tasks(int nb_fc_task, int nb_cl_task)
{
  nb_fc_tasks_enqueued = 0;
  nb_fc_tasks_to_enqueue = nb_fc_task;

  for (int i=0; i<MAX_NB_FC_TASKS && nb_fc_tasks_to_enqueue > 0; i++)
  {
    fc_tasks[i].args[0] = i;
    fc_tasks[i].args[1] = nb_cl_task;
    enqueue_fc_task(&fc_tasks[i]);
  }

  while (nb_fc_tasks_enqueued || nb_fc_tasks_to_enqueue)
  {
    rt_event_yield(NULL);
  }
}

int main()
{
  rt_task_conf_t conf;

  if (rt_event_alloc(NULL, MAX_NB_FC_TASKS + 1)) return -1;

  printf("Starting test\n");

  rt_cluster_mount(1, 0, 0, NULL);

  nb_failures = 0;

  rt_task_conf_init(&conf);
  rt_event_t *event = rt_event_get_blocking(NULL);

  for (int i=0; i<MAX_NB_FC_TASKS; i++)
  {
    fc_stacks_size[i] = 128;
    fc_stacks[i] = rt_alloc(RT_ALLOC_CL_DATA, 128);
    if (fc_stacks[i] == NULL)
      return -1;

    for (int j=0; j<MAX_NB_CL_TASKS; j++)
    {
      stacks_size[i][j] = 128;
      stacks[i][j] = rt_alloc(RT_ALLOC_CL_DATA, 128*((j & (rt_nb_pe()-1)) + 1));
      if (stacks[i][j] == NULL)
        return -1;
    }
  }


  conf.stack_size = 0;

  rt_task_cluster_init(&cluster, &conf, event);

  check_tasks(2, 16);

  if (nb_failures)
  {
    printf("Test FAILURE\n");
    return -1;
  }

  printf("Test SUCCESS\n");

  return 0;
}