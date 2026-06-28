#include "profiler.h"
#include "toolkit.h"

#include <atomic>
#include <thread>

//
// Config
// 
#define RANDOM_SEED 6767
#define CIRCLE_COUNT 100000UL
#define CIRCLE_PER_TASK 8192UL // must be multiple of 8
#define WORKER_THREAD_COUNT 5UL
#define USE_SIMD // comment this to use scalar implementation

//
// Collision detection
//
#if defined(USE_SIMD)

//
// NEON
//
#if defined(__aarch64__) || defined(_M_ARM64)
#define COMPUTE_STEP 4 // no 256-registers :(

//
// Tests 4 circle pairs, returns u32 bitmask
//
inline u32
test_circles_simd(const f32 *x0, const f32 *y0, const f32 *r0, const f32 *x1, const f32 *y1, const f32 *r1)
{
  float32x4_t vx0 = vld1q_f32(x0);
  float32x4_t vy0 = vld1q_f32(y0);
  float32x4_t vx1 = vld1q_f32(x1);
  float32x4_t vy1 = vld1q_f32(y1);
  float32x4_t vr0 = vld1q_f32(r0);
  float32x4_t vr1 = vld1q_f32(r1);

  float32x4_t dx = vsubq_f32(vx1, vx0);
  float32x4_t dy = vsubq_f32(vy1, vy0);
  float32x4_t vdistance_sq = vmlaq_f32(vmulq_f32(dx, dx), dy, dy);

  float32x4_t vrs = vaddq_f32(vr0, vr1);
  float32x4_t vmax_distance_sq = vmulq_f32(vrs, vrs);

  uint32x4_t vcompare = vcltq_f32(vdistance_sq, vmax_distance_sq);
  // bitshift right to get 0b1 in each lane, then extract (kinda _mm256_movemask_ps)
  uint32x4_t mask = vshrq_n_u32(vcompare, 31);
  return vgetq_lane_u32(mask, 0) | (vgetq_lane_u32(mask, 1) << 1) | (vgetq_lane_u32(mask, 2) << 2) |
         (vgetq_lane_u32(mask, 3) << 3);
}
#endif

//
// AVX (assume it's supported on most intel/amd, it's 15 y.o.)
//
#if defined(__x86_64__) || defined(_M_X64)
#define COMPUTE_STEP 8

// Tests 8 circle pairs, returns u32 bitmask
inline u32
test_circles_simd(const f32 *x0, const f32 *y0, const f32 *r0, const f32 *x1, const f32 *y1, const f32 *r1)
{
  __m256 vx0 = _mm256_loadu_ps(x0);
  __m256 vy0 = _mm256_loadu_ps(y0);
  __m256 vx1 = _mm256_loadu_ps(x1);
  __m256 vy1 = _mm256_loadu_ps(y1);
  __m256 vr0 = _mm256_loadu_ps(r0);
  __m256 vr1 = _mm256_loadu_ps(r1);

  __m256 dx = _mm256_sub_ps(vx1, vx0);
  __m256 dy = _mm256_sub_ps(vy1, vy0);
  __m256 distance_sq = _mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy));

  __m256 rs = _mm256_add_ps(vr0, vr1);
  __m256 max_distance_sq = _mm256_mul_ps(rs, rs);

  __m256 compare = _mm256_cmp_ps(distance_sq, max_distance_sq, _CMP_LT_OQ);
  return (u32)_mm256_movemask_ps(compare);
}
#endif

//
// Scalar
//
#else
#define COMPUTE_STEP 1

// Tests 1 circle pair, returns u32 bitmask
inline u32
test_circle_scalar(const f32 *x0, const f32 *y0, const f32 *r0, const f32 *x1, const f32 *y1, const f32 *r1)
{
  f32 dx = *x1 - *x0;
  f32 dy = *y1 - *y0;
  f32 distance_sq = (dx * dx) + (dy * dy);
  f32 max_distance_sq = (*r0 + *r1) * (*r0 + *r1);
  return distance_sq < max_distance_sq ? 0b1 : 0b0;
}
#endif

struct CollisionPair
{
  u32 a_idx;
  u32 b_idx;
};

struct FindCollisionsTask
{
  u32 circle_count;
  u32 compute_first;
  u32 compute_count;
  f32 *ptr_circle_pos_x;
  f32 *ptr_circle_pos_y;
  f32 *ptr_circle_radius;

  u32 max_collision_count;
  std::atomic_uint32_t *ptr_collision_count;
  CollisionPair *ptr_collision_pairs;
};

struct Simulation
{
  u32 circle_count;

  // SoA needed for SIMD
  f32 *ptr_circle_pos_x;
  f32 *ptr_circle_pos_y;
  f32 *ptr_circle_radius;

  u32 max_collision_count;
  std::atomic_uint32_t collision_count;
  CollisionPair *ptr_collisions;

  Simulation(u32 p_circle_count)
  {
    PROFILER_SCOPE("scene_generation");

    Rng rng = Rng(RANDOM_SEED);

    circle_count = p_circle_count;

    // SIMD loads data by COMPUTE_STEP sized chunks, so
    // array needs (COMPUTE_STEP - 1) padding at the end
    // because max b_idx value is a_idx + COMPUTE_STEP
    u32 array_size = circle_count + COMPUTE_STEP - 1;
    ptr_circle_pos_x = new f32[array_size]();
    ptr_circle_pos_y = new f32[array_size]();
    ptr_circle_radius = new f32[array_size]();

    for (u32 idx = 0; idx < circle_count; idx++)
    {
      ptr_circle_pos_x[idx] = (rng.get_f32() * 2.0 - 1.0) * 100.0;
      ptr_circle_pos_y[idx] = (rng.get_f32() * 2.0 - 1.0) * 100.0;
      ptr_circle_radius[idx] = rng.get_f32() * 9.9 + 0.1;
    }

    max_collision_count = circle_count * circle_count;
    collision_count = 0;
    ptr_collisions = new CollisionPair[max_collision_count]();
  }

  ~Simulation()
  {
    delete[] ptr_circle_pos_x;
    delete[] ptr_circle_pos_y;
    delete[] ptr_circle_radius;
    delete[] ptr_collisions;
  }
};

//
// Safe to call from any thread, returns nullptr if there are no available slots
//
CollisionPair *
get_collision_slot(FindCollisionsTask *task)
{
  u32 collision_count = task->ptr_collision_count->load(std::memory_order_relaxed);
  while (collision_count < task->max_collision_count)
  {
    if (task->ptr_collision_count->compare_exchange_weak(
            collision_count, collision_count + 1, std::memory_order_release, std::memory_order_relaxed))
    {
      return &task->ptr_collision_pairs[collision_count];
    }
  }
  return nullptr;
}

void
do_task(FindCollisionsTask *task)
{
  PROFILER_SCOPE(__FUNCTION__);

  u32 a_end = task->compute_first + task->compute_count - COMPUTE_STEP;
  u32 a_idx = task->compute_first;

  for (u32 a_idx = task->compute_first; a_idx <= a_end; a_idx += COMPUTE_STEP)
  {
    PROFILER_SCOPE("circle_collisions");

    // need to be shifted one by one to visit all pairs
    u32 b_end = task->circle_count;
    for (u32 b_idx = a_idx + 1; b_idx < b_end; b_idx++)
    {

#if defined(USE_SIMD)
      u32 cmp_result = test_circles_simd(task->ptr_circle_pos_x + a_idx, task->ptr_circle_pos_y + a_idx,
          task->ptr_circle_radius + a_idx, task->ptr_circle_pos_x + b_idx, task->ptr_circle_pos_y + b_idx,
          task->ptr_circle_radius + b_idx);

      u32 valid_pair_count = min(b_end - b_idx, COMPUTE_STEP);
      for (u32 pair_idx = 0; pair_idx < valid_pair_count; pair_idx++)
      {
        if (((cmp_result >> pair_idx) & 0b1) == 0b1)
        {
          CollisionPair *collision_slot = get_collision_slot(task);
          if (nullptr != collision_slot)
          {
            collision_slot->a_idx = a_idx + pair_idx;
            collision_slot->b_idx = b_idx + pair_idx;
          }
          else
          {
            return; // no slots, remainging checks are redundant
          }
        }
      }

#else // Scalar fallback
      u32 result = test_circle_scalar(task->ptr_circle_pos_x + a_idx, task->ptr_circle_pos_y + a_idx,
          task->ptr_circle_radius + a_idx, task->ptr_circle_pos_x + b_idx, task->ptr_circle_pos_y + b_idx,
          task->ptr_circle_radius + b_idx);
      if ((result & 0b1) == 0b1)
      {
        CollisionPair *collision_slot = get_collision_slot(task);
        if (nullptr != collision_slot)
        {
          collision_slot->a_idx = a_idx;
          collision_slot->b_idx = b_idx;
        }
        else
        {
          return; // no slots, remainging checks are redundant
        }
      }
#endif
    }
  }
}

class WorkQueue
{
private:
  std::atomic_uint32_t in_progress_task_count;
  std::atomic_uint32_t queued_task_count;
  u32 max_task_count;
  FindCollisionsTask *ptr_tasks;

public:
  WorkQueue(u32 p_max_task_count)
  {
    max_task_count = p_max_task_count;
    in_progress_task_count = 0;
    queued_task_count = 0;
    ptr_tasks = new FindCollisionsTask[max_task_count]();

    log_info("work queue created with {} slots", max_task_count);
  }

  ~WorkQueue()
  {
    delete[] ptr_tasks;
  }

  //
  // Safe to call ONLY from the main thread (task data write is NOT atomic)
  //
  void
  push_task(FindCollisionsTask &&task)
  {
    if (queued_task_count < max_task_count)
    {
      log_info("task({}, {}) pushed to queue", task.compute_first, task.compute_first + task.compute_count);

      u32 task_idx = queued_task_count.load(std::memory_order_relaxed);
      ptr_tasks[task_idx] = task;
      queued_task_count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  //
  // Safe to call from any thread, returns nullptr if queue is empty
  //
  FindCollisionsTask *
  get_task()
  {
    u32 task_count = queued_task_count.load(std::memory_order_relaxed);
    while (task_count > 0)
    {
      // This will reload actual count value on each failed attempt
      if (queued_task_count.compare_exchange_weak(
              task_count, task_count - 1, std::memory_order_release, std::memory_order_relaxed))
      {
        u32 ip_count = in_progress_task_count.fetch_add(1, std::memory_order_relaxed) + 1;
        u32 task_idx = task_count - 1;
        FindCollisionsTask *task = &ptr_tasks[task_idx];
        return task;
      }
    }

    return nullptr;
  }

  void
  report_task_finished()
  {
    u32 ip_count = in_progress_task_count.fetch_sub(1, std::memory_order_relaxed) - 1;
  }

  u32
  get_queued_task_count()
  {
    return queued_task_count.load(std::memory_order_relaxed);
  }

  u32
  get_in_progress_task_count()
  {
    return in_progress_task_count.load(std::memory_order_relaxed);
  }
};

struct WorkerContext
{
  u32 logical_id;
  WorkQueue *ptr_queue;
};

void
worker_proc(WorkerContext *ctx)
{
  WorkQueue *queue = ctx->ptr_queue;
  for (;;)
  {
    FindCollisionsTask *task = queue->get_task();
    if (nullptr != task)
    {
      log_raw("[worker_{}]  doing task({}, {})", ctx->logical_id, task->compute_first,
          task->compute_first + task->compute_count);

      do_task(task);

      log_raw("[worker_{}]  task finished", ctx->logical_id);
      queue->report_task_finished();
    }
    else
    {
      log_raw("[worker_{}]  has no work to do, exiting", ctx->logical_id);
      return;
    }
  }
}

struct Workers
{
  WorkerContext worker_context_array[WORKER_THREAD_COUNT];
  std::thread worker_thread_array[WORKER_THREAD_COUNT];
};

struct State
{
  Workers workers;
  WorkQueue *ptr_work_queue;
  Simulation *ptr_simulation;

  State()
  {
    u32 task_count = (CIRCLE_COUNT / CIRCLE_PER_TASK) + 1;
    ptr_work_queue = new WorkQueue(task_count);
    ptr_simulation = new Simulation(CIRCLE_COUNT);
  }

  ~State()
  {
    delete ptr_work_queue;
    delete ptr_simulation;
  }
};

void
profiler_print_records();

void
simulation_run(State *state)
{
  PROFILER_SCOPE(__FUNCTION__);

  WorkQueue *queue = state->ptr_work_queue;
  Simulation *simulation = state->ptr_simulation;

  {
    PROFILER_SCOPE("prepare_tasks");

    u32 base_idx = 0;
    while (base_idx < CIRCLE_COUNT)
    {
      u32 compute_count = min(CIRCLE_COUNT - base_idx, CIRCLE_PER_TASK);
      queue->push_task({
          .circle_count = simulation->circle_count,
          .compute_first = base_idx,
          .compute_count = compute_count,
          .ptr_circle_pos_x = simulation->ptr_circle_pos_x,
          .ptr_circle_pos_y = simulation->ptr_circle_pos_y,
          .ptr_circle_radius = simulation->ptr_circle_radius,
          .max_collision_count = simulation->max_collision_count,
          .ptr_collision_count = &simulation->collision_count,
          .ptr_collision_pairs = simulation->ptr_collisions,
      });
      base_idx += CIRCLE_PER_TASK;
    }
  }

  Workers *workers = &state->workers;
  for (u32 idx = 0; idx < WORKER_THREAD_COUNT; idx++)
  {
    WorkerContext *worker_ctx = &workers->worker_context_array[idx];
    worker_ctx->logical_id = idx;
    worker_ctx->ptr_queue = state->ptr_work_queue;
    workers->worker_thread_array[idx] = std::thread(worker_proc, worker_ctx);
    workers->worker_thread_array[idx].detach(); // don't need to join threads
  }

  // Spin and do work if any, until queue is empty and all started tasks are finished
  while (queue->get_queued_task_count() > 0 || queue->get_in_progress_task_count() > 0)
  {
    FindCollisionsTask *task = queue->get_task();
    if (nullptr != task)
    {
      log_raw("[main_thread]  doing task({}, {})", task->compute_first, task->compute_first + task->compute_count);

      do_task(task);
      queue->report_task_finished();

      log_raw("[main_thread]  task finished");
    }
  }

#if defined(USE_SIMD)
  log_raw("\n======== SIMULATION RESULTS (SIMD) ========");
#else
  log_raw("\n======== SIMULATION RESULTS (SCALAR) ========");
#endif
  log_raw("{} circles", simulation->circle_count);
  log_raw("{} collisions", simulation->collision_count.load());
}

i32
main()
{
  profiler_initialize();

  State state = State();
  simulation_run(&state);

  profiler_print_records();
  return 0;
}

PROFILER_RECORDS_ARRAY;

void
profiler_print_records()
{
  log_raw("\n======== PROFILER RECORDS ========");
  for (u32 i = 0; i < array_len(debug_records); i++)
  {
    DebugRecord *record = &debug_records[i];
    u32 hits = record->hits.load(std::memory_order_relaxed);
    u64 ticks = record->ticks.load(std::memory_order_relaxed);

    u64 ticks_per_hit = (ticks / (u64)hits);
    f32 time_total_ms = milliseconds_from_ticks(ticks);
    f32 time_per_hit_ms = milliseconds_from_ticks(ticks_per_hit);

    string location = std::format("{}:{}", record->file, record->line);
    string title = std::format("[{}]", record->name);
    log_raw("{:<16} {:>32}::{:>16.4f} ms {:>10} hits {:>16.8f} ms/hit", //
        location, title, time_total_ms, hits, time_per_hit_ms);
  }
}
