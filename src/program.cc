/*
 * Copyright 2015 Stanford University and NVIDIA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
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

#include "weft.h"
#include "race.h"
#include "program.h"
#include "instruction.h"

#include <fstream>
#include <iostream>
#include <vector>

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>

Program::Program(Weft *w)
  : weft(w)
{
}

Program::Program(const Program &rhs)
  : weft(NULL)
{
  // should never be called
  assert(false);
}

Program::~Program(void)
{
  for (std::vector<PTXInstruction*>::iterator it = 
        ptx_instructions.begin(); it != ptx_instructions.end(); it++)
  {
    delete (*it);
  }
  ptx_instructions.clear();
}

Program& Program::operator=(const Program &rhs)
{
  // should never be called
  assert(false);
  return *this;
}

void Program::parse_ptx_file(const char *file_name, int &max_num_threads)
{
  std::ifstream file(file_name);
  std::vector<std::pair<std::string,int> > lines;
  // First, let's get all the lines we care about
  if (file.is_open())
  {
    bool start_recording = false;
    bool found = false;
    std::string line;
    int line_num = 1;
    while (std::getline(file, line))
    {
      if (start_recording)
        lines.push_back(std::pair<std::string,int>(line,line_num));
      if (line.find(".entry") != std::string::npos)
      {
        // We should only have one entry kernel, we don't know
        // how to do this for more than one kernel at the moment
        if (start_recording)
        {
          char buffer[1024];
          snprintf(buffer, 1023, "Found multiple entry kernels in file %s. "
                                 "Weft currently only supports one kernel "
                                 "per file.", file_name);
          weft->report_error(WEFT_ERROR_MULTIPLE_KERNELS, buffer);
        }
        start_recording = true;
      }
      if (!found && (line.find(".maxntid") != std::string::npos))
      {
        int temp = atoi(line.substr(line.find(" "),line.find(",")).c_str());
        if (max_num_threads != -1)
        {
          if (temp != max_num_threads)
          {
            char buffer[1024];
            snprintf(buffer, 1023, "Found max thread count %d "
                           "which does not agree with specified count "
                           "of %d", temp, max_num_threads);
            weft->report_error(WEFT_ERROR_THREAD_COUNT_MISMATCH, buffer);
          }
        }
        else
          max_num_threads = temp;
        found = true;
      }
      line_num++;
    }
  }
  else
  {
    char buffer[1024];
    snprintf(buffer, 1023, "Unable to open file %s", file_name);
    weft->report_error(WEFT_ERROR_FILE_OPEN, buffer);
  }
  // Once we have the lines, then convert them into static PTX instructions
  convert_to_instructions(max_num_threads, lines);
}

void Program::report_statistics(void)
{
  fprintf(stdout,"WEFT INFO: Program Statistics\n");
  fprintf(stdout,"  Static Instructions: %ld\n", ptx_instructions.size());
  fprintf(stdout,"  Instruction Counts\n");
  unsigned counts[PTX_LAST];
  for (unsigned idx = 0; idx < PTX_LAST; idx++)
    counts[idx] = 0;
  for (std::vector<PTXInstruction*>::const_iterator it = 
        ptx_instructions.begin(); it != ptx_instructions.end(); it++)
    counts[(*it)->get_kind()]++;
  for (unsigned idx = 0; idx < PTX_LAST; idx++)
  {
    if (counts[idx] == 0)
      continue;
    fprintf(stdout,"    Instruction %s: %d\n", 
                   PTXInstruction::get_kind_name((PTXKind)idx), counts[idx]);
  }
  fprintf(stdout,"\n");
}

void Program::report_statistics(const std::vector<Thread*> &threads)
{
  int total_count = 0;
  std::vector<int> instruction_counts(PTX_LAST, 0);
  for (std::vector<Thread*>::const_iterator it = threads.begin();
        it != threads.end(); it++)
  {
    total_count += (*it)->accumulate_instruction_counts(instruction_counts);
  }
  fprintf(stdout,"WEFT INFO: Program Statistics\n");
  fprintf(stdout,"  Dynamic Instructions: %d\n", total_count);
  fprintf(stdout,"  Instruction Counts\n");
  for (unsigned idx = 0; idx < PTX_LAST; idx++)
  {
    if (instruction_counts[idx] == 0)
      continue;
    fprintf(stdout,"    Instruction %s: %d\n", 
       PTXInstruction::get_kind_name((PTXKind)idx), instruction_counts[idx]);
  }
  fprintf(stdout,"\n");
}

bool Program::has_shuffles(void) const
{
  for (std::vector<PTXInstruction*>::const_iterator it = 
        ptx_instructions.begin(); it != ptx_instructions.end(); it++)
  {
    if ((*it)->is_shuffle())
      return true;
  }
  return false;
}

int Program::emulate(Thread *thread)
{
  int dynamic_instructions = 0;
  PTXInstruction *pc = ptx_instructions.front();
  bool profile = weft->print_verbose();
  if (profile)
  {
    while (pc != NULL)
    {
      thread->profile_instruction(pc);
      pc = pc->emulate(thread);
      dynamic_instructions++;
    }
  }
  else
  {
    while (pc != NULL)
    {
      pc = pc->emulate(thread);
      dynamic_instructions++;
    }
  }
  return dynamic_instructions;
}

void Program::emulate_warp(Thread **threads)
{
  // Execute all the threads in lock-step
  PTXInstruction *pc = ptx_instructions.front();  
  ThreadState thread_state[WARP_SIZE];
  for (int i = 0; i < WARP_SIZE; i++)
    thread_state[i] = ThreadState();
  int dynamic_instructions[WARP_SIZE];
  for (int i = 0; i < WARP_SIZE; i++)
    dynamic_instructions[i] = 0;
  int shared_access_id = 0;
  bool profile = weft->print_verbose();
  if (profile)
  {
    while (pc != NULL)
    {
      for (int i = 0; i < WARP_SIZE; i++)
      {
        if (thread_state[i].status == THREAD_ENABLED)
        {
          threads[i]->profile_instruction(pc);
          dynamic_instructions[i]++;
        }
      }
      pc = pc->emulate_warp(threads, thread_state, shared_access_id);
    }
  }
  else
  {
    while (pc != NULL)
    {
      for (int i = 0; i < WARP_SIZE; i++)
      {
        if (thread_state[i].status == THREAD_ENABLED)
          dynamic_instructions[i]++;
      }
      pc = pc->emulate_warp(threads, thread_state, shared_access_id);
    }
  }
  for (int i = 0; i < WARP_SIZE; i++)
    threads[i]->set_dynamic_instructions(dynamic_instructions[i]);
}

void Program::convert_to_instructions(int max_num_threads,
                const std::vector<std::pair<std::string,int> > &lines)
{
  // Make a first pass and create all the instructions
  // Track all the basic block program counters
  std::map<std::string,PTXInstruction*> labels;
  PTXInstruction *previous = NULL;
  for (std::vector<std::pair<std::string,int> >::const_iterator it = 
        lines.begin(); it != lines.end(); it++)
  {
    PTXInstruction *next = PTXInstruction::interpret(it->first, it->second);
    // Skip any empty lines
    if (next == NULL)
      continue;
    ptx_instructions.push_back(next);
    if (next->is_label())
    {
      PTXLabel *label = next->as_label();
      label->update_labels(labels);
    }
    if (previous != NULL)
      previous->set_next(next);
    previous = next;
  }
  // Then make a second pass to fill in the pointers
  for (std::vector<PTXInstruction*>::const_iterator it = 
        ptx_instructions.begin(); it != ptx_instructions.end(); it++)
  {
    if ((*it)->is_branch())
    {
      PTXBranch *branch = (*it)->as_branch();
      branch->set_targets(labels);
    }
    if ((*it)->is_barrier())
    {
      PTXBarrier *barrier = (*it)->as_barrier();
      barrier->update_count(max_num_threads);
    }
  }
}

Thread::Thread(unsigned tid, Program *p, SharedMemory *m)
  : thread_id(tid), program(p), shared_memory(m), 
    max_barrier_name(-1), dynamic_instructions(0)
{
  dynamic_counts.resize(PTX_LAST, 0);
}

Thread::~Thread(void)
{
  // Clean up our instructions
  for (std::vector<WeftInstruction*>::iterator it = 
        instructions.begin(); it != instructions.end(); it++)
  {
    delete (*it);
  }
  instructions.clear();
  for (std::deque<Happens*>::iterator it = 
        all_happens.begin(); it != all_happens.end(); it++)
  {
    delete (*it);
  }
  all_happens.clear();
}

void Thread::initialize(void)
{
  // Before starting emulation fill in the special
  // values for particular registers
  register_store[WEFT_TID_REG] = thread_id;
  // Use 0 as the default CTA ID
  register_store[WEFT_CTA_REG] = 0;
}

void Thread::emulate(void)
{
  dynamic_instructions = program->emulate(this);
}

void Thread::cleanup(void)
{
  // Once we are done we can clean up all our data structures
  shared_locations.clear();
  register_store.clear();
  predicate_store.clear();
  globals.clear();
}

void Thread::register_shared_location(const std::string &name, int64_t addr)
{
  assert(shared_locations.find(name) == shared_locations.end());
  shared_locations[name] = addr;
}

bool Thread::find_shared_location(const std::string &name, int64_t &addr)
{
  std::map<std::string,int64_t>::const_iterator finder = 
    shared_locations.find(name);
  if (finder == shared_locations.end()) {
    if (program->weft->report_warnings())
    {
      fprintf(stderr,"WEFT WARNING: Unable to find shared "
                     "memory location %s\n", name.c_str());
    }
    return false;
  }
  addr = finder->second;
  return true;
}

void Thread::register_global_location(const char *name, const int *data, size_t size)
{
  GlobalDataInfo info;
  info.name = name;
  info.data = data;
  info.size = size;
  globals.push_back(info);
}

int64_t Thread::find_global_location(const char *name, bool &valid)
{
  for (unsigned idx = 0; idx < globals.size(); idx++)
  {
    // See if the names match
    if (strcmp(name, globals[idx].name) == 0)
    {
      valid = true;
      return (idx*SDDRINC);
    }
  }
  valid = false;
  return (-1 * SDDRINC);
}

int64_t Thread::find_global_value(int64_t addr, bool &valid)
{
  unsigned index = addr / SDDRINC;
  if ((index >= 0) && (index < globals.size()))
  {
    valid = true;
    size_t offset = addr - (index * SDDRINC);
    assert(offset < globals[index].size);
    valid = true;
    return globals[index].data[offset];
  }
  valid = false;
  return 0;
}

void Thread::set_value(int64_t reg, int64_t value)
{
  register_store[reg] = value;
}

bool Thread::get_value(int64_t reg, int64_t &value)
{
  std::map<int64_t,int64_t>::const_iterator finder = 
    register_store.find(reg);
  if (finder == register_store.end()) {
    if (program->weft->report_warnings())
    {
      char buffer[11];
      PTXInstruction::decompress_identifier(reg, buffer, 11);
      fprintf(stderr,"WEFT WARNING: Unable to find register %s\n", buffer);
    }
    return false;
  }
  value = finder->second;
  return true;
}

void Thread::set_pred(int64_t pred, bool value)
{
  predicate_store[pred] = value;
}

bool Thread::get_pred(int64_t pred, bool &value)
{
  std::map<int64_t,bool>::const_iterator finder = 
    predicate_store.find(pred);
  if (finder == predicate_store.end()) {
    if (program->weft->report_warnings())
    {
      char buffer[11];
      PTXInstruction::decompress_identifier(pred, buffer, 11);
      fprintf(stderr,"WEFT WARNING: Unable to find predicate %s\n", buffer);
    }
    return false;
  }
  value = finder->second;
  return true;
}

void Thread::add_instruction(WeftInstruction *instruction)
{
  instructions.push_back(instruction);
}

void Thread::update_max_barrier_name(int name)
{
  if (name > max_barrier_name)
    max_barrier_name = name;
}

void Thread::profile_instruction(PTXInstruction *instruction)
{
  int kind = instruction->get_kind();  
  dynamic_counts[kind]++;
}

int Thread::accumulate_instruction_counts(std::vector<int> &total_counts)
{
  int total = 0; 
  assert(total_counts.size() == dynamic_counts.size());
  for (unsigned idx = 0; idx < total_counts.size(); idx++)
  {
    total_counts[idx] += dynamic_counts[idx];
    total += dynamic_counts[idx];
  }
  return total;
}

void Thread::update_shared_memory(WeftAccess *access)
{
  shared_memory->update_accesses(access);
}

void Thread::initialize_happens(int total_threads,
                                int max_num_barriers)
{
  initialize_happens_instances(total_threads); 
  compute_barriers_before(max_num_barriers);
  compute_barriers_after(max_num_barriers);
}

void Thread::update_happens_relationships(void)
{
  for (std::deque<Happens*>::const_iterator it = 
        all_happens.begin(); it != all_happens.end(); it++)
  {
    (*it)->update_happens_relationships();
  }
}

void Thread::initialize_happens_instances(int total_threads)
{
  // First create
  Happens *next = NULL;  
  for (std::vector<WeftInstruction*>::const_iterator it = 
        instructions.begin(); it != instructions.end(); it++)
  {
    // Don't make happens for barriers
    if ((*it)->is_barrier())
    {
      next = NULL;
      continue;
    }
    if (next == NULL)
    {
      next = new Happens(total_threads);
      all_happens.push_back(next);
    }
    (*it)->initialize_happens(next);
  }
}

void Thread::compute_barriers_before(int max_num_barriers)
{
  std::vector<WeftBarrier*> before_barriers(max_num_barriers, NULL);
  bool has_update = false;
  for (std::vector<WeftInstruction*>::const_iterator it = 
        instructions.begin(); it != instructions.end(); it++)
  {
    // We only count syncs in the set of barriers before
    // because they are the only instructions which can 
    // establish a happens-before relationship. On the
    // contrary, arrives can always establish a happens-after.
    if ((*it)->is_sync())
    {
      WeftBarrier *bar = (*it)->as_barrier(); 
      assert(bar->name < max_num_barriers);
      before_barriers[bar->name] = bar;
      has_update = true;
    }
    else if ((*it)->is_arrive())
      has_update = true; // set to true to update next happens
    else if (has_update)
    {
      Happens *happens = (*it)->get_happens();
      assert(happens != NULL);
      happens->update_barriers_before(before_barriers);
      has_update = false;
    }
  }
}

void Thread::compute_barriers_after(int max_num_barriers)
{
  std::vector<WeftBarrier*> after_barriers(max_num_barriers, NULL);
  bool has_update = false;
  for (std::vector<WeftInstruction*>::reverse_iterator it = 
        instructions.rbegin(); it != instructions.rend(); it++)
  {
    if ((*it)->is_barrier())
    {
      WeftBarrier *bar = (*it)->as_barrier();
      assert(bar->name < max_num_barriers);
      after_barriers[bar->name] = bar;
      has_update = true;
    }
    else if (has_update)
    {
      Happens *happens = (*it)->get_happens();
      assert(happens != NULL);
      happens->update_barriers_after(after_barriers);
      has_update = false;
    }
  }
}

EmulateThread::EmulateThread(Thread *t)
  : WeftTask(), thread(t)
{
}

void EmulateThread::execute(void)
{
  thread->initialize();
  thread->emulate();
  thread->cleanup();
}

EmulateWarp::EmulateWarp(Program *p, Thread **start)
  : WeftTask(), program(p), threads(start)
{
}

void EmulateWarp::execute(void)
{
  // Initialize all the threads
  for (int i = 0; i < WARP_SIZE; i++)
    threads[i]->initialize();

  // Have the program simulate all the threads together
  program->emulate_warp(threads);

  // Cleanup all the threads
  for (int i = 0; i < WARP_SIZE; i++)
    threads[i]->cleanup();
}

InitializationTask::InitializationTask(Thread *t, int total, int max_barriers)
  : WeftTask(), thread(t), total_threads(total), max_num_barriers(max_barriers)
{
}

void InitializationTask::execute(void)
{
  thread->initialize_happens(total_threads, max_num_barriers);
}

UpdateThreadTask::UpdateThreadTask(Thread *t)
  : WeftTask(), thread(t)
{
}

void UpdateThreadTask::execute(void)
{
  thread->update_happens_relationships();
}

