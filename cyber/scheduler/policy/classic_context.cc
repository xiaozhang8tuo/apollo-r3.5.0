/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "cyber/scheduler/policy/classic_context.h"

#include "cyber/event/perf_event_cache.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using apollo::cyber::croutine::RoutineState;
using apollo::cyber::base::ReadLockGuard;
using apollo::cyber::base::AtomicRWLock;
using apollo::cyber::croutine::CRoutine;
using apollo::cyber::event::PerfEventCache;
using apollo::cyber::event::SchedPerf;

namespace {
static constexpr auto MIN_SLEEP_INTERVAL = std::chrono::milliseconds(1);
}

GRP_WQ_MUTEX ClassicContext::mtx_wq_;
GRP_WQ_CV ClassicContext::cv_wq_;
RQ_LOCK_GROUP ClassicContext::rq_locks_;
CR_GROUP ClassicContext::cr_group_;

std::shared_ptr<CRoutine> ClassicContext::NextRoutine() {
  if (unlikely(stop_)) {
    return nullptr;
  }

  for (int i = MAX_PRIO - 1; i >= 0; --i) { // 在自己绑定的协程组(group_name)中，从高到低去协程组的优先级队列中取任务 19是最高优先级 0 最低
    ReadLockGuard<AtomicRWLock> lk(rq_locks_[group_name_].at(i));
    for (auto& cr : cr_group_[group_name_].at(i)) { // 从相同优先级队列中 遍历协程任务
      if (!cr->Acquire()) {
        continue;
      }

      //对于DataVistor来说，有数据后，TryFetch成功才会置为就绪态，可以执行读回调
      if (cr->UpdateState() == RoutineState::READY) {
        PerfEventCache::Instance()->AddSchedEvent(SchedPerf::NEXT_RT, cr->id(),
                                                  cr->processor_id());
        return cr;
      }

      if (unlikely(cr->state() == RoutineState::SLEEP)) {
        if (!need_sleep_ || wake_time_ > cr->wake_time()) { //取较小的时间，醒来后肯定有任务能执行
          need_sleep_ = true;
          wake_time_ = cr->wake_time();
        }
      }

      cr->Release();
    }
  }

  return nullptr;
}

void ClassicContext::Wait() {
  std::unique_lock<std::mutex> lk(mtx_wq_[group_name_]);
  if (stop_) {
    return;
  }

  if (unlikely(need_sleep_)) {
    auto duration = wake_time_ - std::chrono::steady_clock::now();
    cv_wq_[group_name_].wait_for(lk, duration);//醒来就有任务执行，wake_time_取的是协程中的最小唤醒时间
    need_sleep_ = false;
  } else {
    cv_wq_[group_name_].wait(lk);
  }
}

void ClassicContext::Shutdown() {
  {
    std::lock_guard<std::mutex> lg(mtx_wq_[group_name_]);
    if (!stop_) {
      stop_ = true;
    }
  }
  cv_wq_[group_name_].notify_all();
}

void ClassicContext::Notify(const std::string& group_name) {
  cv_wq_[group_name].notify_one();
}

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo
