//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2020 QMCPACK developers.
//
// File developed by: Ken Esler, kpesler@gmail.com, University of Illinois at Urbana-Champaign
//                    Jeongnim Kim, jeongnim.kim@gmail.com, University of Illinois at Urbana-Champaign
//                    Jeremy McMinnis, jmcminis@gmail.com, University of Illinois at Urbana-Champaign
//
// File created by: Ken Esler, kpesler@gmail.com, University of Illinois at Urbana-Champaign
//////////////////////////////////////////////////////////////////////////////////////


/** @file TimerManager.h
 * @brief TimerManager class.
 */
#ifndef QMCPLUSPLUS_TIMER_MANAGER_H
#define QMCPLUSPLUS_TIMER_MANAGER_H

#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include "NewTimer.h"
#include "config.h"
#include "OhmmsData/Libxml2Doc.h"

#ifdef USE_VTUNE_TASKS
#include <ittnotify.h>
#endif

class Communicate;

namespace qmcplusplus
{

class TimerManagerClass
{
protected:
  std::vector<std::unique_ptr<NewTimer>> TimerList;
  std::vector<NewTimer*> CurrentTimerStack;
  timer_levels timer_threshold;
  timer_id_t max_timer_id;
  bool max_timers_exceeded;
  std::map<timer_id_t, std::string> timer_id_name;
  std::map<std::string, timer_id_t> timer_name_to_id;

  void initializeTimer(NewTimer* t);

public:
#ifdef USE_VTUNE_TASKS
  __itt_domain* task_domain;
#endif

  TimerManagerClass() : timer_threshold(timer_level_coarse), max_timer_id(1), max_timers_exceeded(false)
  {
#ifdef USE_VTUNE_TASKS
    task_domain = __itt_domain_create("QMCPACK");
#endif
  }

  NewTimer* createTimer(const std::string& myname, timer_levels mytimer = timer_level_fine);

  void push_timer(NewTimer* t)
  {
    {
      CurrentTimerStack.push_back(t);
    }
  }

  void pop_timer()
  {
    {
      CurrentTimerStack.pop_back();
    }
  }

  NewTimer* current_timer()
  {
    NewTimer* current = NULL;
    if (CurrentTimerStack.size() > 0)
    {
      current = CurrentTimerStack.back();
    }
    return current;
  }

  void set_timer_threshold(const timer_levels threshold);

  bool maximum_number_of_timers_exceeded() const { return max_timers_exceeded; }

  void reset();
  void print(Communicate* comm);
  void print_flat(Communicate* comm);
  void print_stack(Communicate* comm);

  typedef std::map<std::string, int> nameList_t;
  typedef std::vector<double> timeList_t;
  typedef std::vector<long> callList_t;
  typedef std::vector<std::string> names_t;

  struct FlatProfileData
  {
    nameList_t nameList;
    timeList_t timeList;
    callList_t callList;
  };

  struct StackProfileData
  {
    names_t names;
    nameList_t nameList;
    timeList_t timeList;
    timeList_t timeExclList;
    callList_t callList;
  };

  void collate_flat_profile(Communicate* comm, FlatProfileData& p);

  void collate_stack_profile(Communicate* comm, StackProfileData& p);

  void output_timing(Communicate* comm, Libxml2Document& doc, xmlNodePtr root);

  void get_stack_name_from_id(const StackKey& key, std::string& name);

};

extern TimerManagerClass TimerManager;

// Helpers to make it easier to define a set of timers
// See tests/test_timer.cpp for an example

typedef std::vector<NewTimer*> TimerList_t;

template<class T>
struct TimerIDName_t
{
  T id;
  const std::string name;
};

template<class T>
using TimerNameList_t = std::vector<TimerIDName_t<T>>;

template<class T>
void setup_timers(TimerList_t& timers, TimerNameList_t<T> timer_list, timer_levels timer_level = timer_level_fine)
{
  timers.resize(timer_list.size());
  for (int i = 0; i < timer_list.size(); i++)
  {
    timers[timer_list[i].id] = TimerManager.createTimer(timer_list[i].name, timer_level);
  }
}

} // namespace qmcplusplus
#endif
