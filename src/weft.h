
#ifndef __WEFT_H__
#define __WEFT_H__

#include <cstdio>
#include <cassert>
#include <pthread.h>
#include <deque>
#include <vector>

#define PTHREAD_SAFE_CALL(cmd)        \
  {                                   \
    int ret = (cmd);                  \
    if (ret != 0) {                   \
      fprintf(stderr,"PTHREAD error: %s = %d (%s)\n", #cmd, ret, strerror(ret)); \
      assert(false);                  \
    }                                 \
  }

enum {
  WEFT_SUCCESS,
  WEFT_ERROR_NO_FILE_NAME,
  WEFT_ERROR_FILE_OPEN,
  WEFT_ERROR_THREAD_COUNT_MISMATCH,
  WEFT_ERROR_NO_THREAD_COUNT,
  WEFT_ERROR_ARRIVAL_MISMATCH,
  WEFT_ERROR_TOO_MANY_PARTICIPANTS,
  WEFT_ERROR_ALL_ARRIVALS,
  WEFT_ERROR_DEADLOCK,
  WEFT_ERROR_GRAPH_VALIDATION,
};

class Program;
class Thread;
class BarrierDependenceGraph;

class WeftTask {
public:
  virtual ~WeftTask(void) { }
  virtual void execute(void) = 0;
};

class EmulateTask : public WeftTask {
public:
  EmulateTask(Thread *thread);
  EmulateTask(const EmulateTask &rhs) : thread(NULL) { assert(false); }
  virtual ~EmulateTask(void) { }
public:
  EmulateTask& operator=(const EmulateTask &rhs) { assert(false); return *this; }
public:
  virtual void execute(void);
public:
  Thread *const thread;
};

class ValidationTask : public WeftTask {
public:
  ValidationTask(BarrierDependenceGraph *graph, int name, int generation);
  ValidationTask(const ValidationTask &rhs) : graph(NULL), 
    name(0), generation(0) { assert(false); }
  virtual ~ValidationTask(void) { }
public:
  ValidationTask& operator=(const ValidationTask &rhs) { assert(false); return *this; }
public:
  virtual void execute(void);
public:
  BarrierDependenceGraph *const graph;
  const int name;
  const int generation;
};

class Weft {
public:
  Weft(int argc, char **argv);
  ~Weft(void);
public:
  void verify(void);
  void report_error(int error_code, const char *message);
  inline bool report_warnings(void) const { return warnings; }
  inline int barrier_upper_bound(void) const { return max_num_barriers; }
  inline bool print_verbose(void) const { return verbose; }
protected:
  void parse_inputs(int argc, char **argv);
  void report_usage(int error, const char *error_str);
  void parse_ptx(void);
  void emulate_threads(void); 
  void construct_dependence_graph(void);
  void compute_happens_relationships(void);
  void check_for_race_conditions(void);
protected:
  void start_threadpool(void);
  void stop_threadpool(void);
  void initialize_count(unsigned count);
  void wait_until_done(void);
public:
  void enqueue_task(WeftTask *task);
  WeftTask* dequeue_task(void);
  void complete_task(WeftTask *task);
public:
  static void* worker_loop(void *arg);
  static unsigned long long get_current_time_in_micros(void);
  static size_t get_memory_usage(void);
protected:
  void start_instrumentation(int stage);
  void stop_instrumentation(int stage);
  void report_instrumentation(void);
protected:
  const char *file_name;
  int max_num_threads;
  int thread_pool_size;
  int max_num_barriers;
  bool verbose;
  bool instrument;
  bool warnings;
protected:
  Program *program;
  std::vector<Thread*> threads;
protected:
  BarrierDependenceGraph *graph;
protected:
  pthread_t *worker_threads;
protected:
  pthread_mutex_t count_lock;
  pthread_cond_t count_cond;
  unsigned int pending_count;
protected:
  pthread_mutex_t queue_lock;
  pthread_cond_t queue_cond;
  std::deque<WeftTask*> queue;
protected:
  // Instrumentation
  unsigned long long timing[5];
  size_t memory_usage[5];
};

#endif // __WEFT_H__
