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

#include "cyber/scheduler/policy/scheduler_classic.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "cyber/common/environment.h"
#include "cyber/common/file.h"
#include "cyber/event/perf_event_cache.h"
#include "cyber/scheduler/policy/classic_context.h"
#include "cyber/scheduler/processor.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using apollo::cyber::croutine::RoutineState;
using apollo::cyber::base::ReadLockGuard;
using apollo::cyber::base::WriteLockGuard;
using apollo::cyber::common::GlobalData;
using apollo::cyber::common::GetAbsolutePath;
using apollo::cyber::common::PathExists;
using apollo::cyber::common::GetProtoFromFile;
using apollo::cyber::common::WorkRoot;
using apollo::cyber::event::PerfEventCache;
using apollo::cyber::event::SchedPerf;

SchedulerClassic::SchedulerClassic() {
  // get sched config
  std::string conf("conf/");
  conf.append(GlobalData::Instance()->ProcessGroup()).append(".conf");
  auto cfg_file = GetAbsolutePath(WorkRoot(), conf);

  apollo::cyber::proto::CyberConfig cfg;
  if (PathExists(cfg_file) && GetProtoFromFile(cfg_file, &cfg)) {
    classic_conf_ = cfg.scheduler_conf().classic_conf();
    for (auto& group : classic_conf_.groups()) {
      auto& group_name = group.name();
      for (auto task : group.tasks()) {
        task.set_group_name(group_name);
        cr_confs_[task.name()] = task;
      }
    }
  } else {
    // if do not set default_proc_num in scheduler conf
    // give a default value
    uint32_t proc_num = 2;
    auto& global_conf = GlobalData::Instance()->Config();
    if (global_conf.has_scheduler_conf() &&
        global_conf.scheduler_conf().has_default_proc_num()) {
      proc_num = global_conf.scheduler_conf().default_proc_num();
    }
    task_pool_size_ = proc_num;

    auto sched_group = classic_conf_.add_groups();
    sched_group->set_name(DEFAULT_GROUP_NAME);
    sched_group->set_processor_num(proc_num);
  }

  CreateProcessor();
}

void SchedulerClassic::CreateProcessor() {
  for (auto& group : classic_conf_.groups()) {//???????????????????????????????????????????????? 
    auto& group_name = group.name();
    auto proc_num = group.processor_num();
    if (task_pool_size_ == 0) {
      task_pool_size_ = proc_num;
    }

    auto& affinity = group.affinity();
    auto& processor_policy = group.processor_policy();
    auto processor_prio = group.processor_prio();
    std::vector<int> cpuset;
    ParseCpuset(group.cpuset(), &cpuset);

    ClassicContext::cr_group_[group_name]; //??????????????????/cv/mtx ??? ??? ???Processor?????????
    ClassicContext::rq_locks_[group_name];
    ClassicContext::mtx_wq_[group_name];
    ClassicContext::cv_wq_[group_name];

    for (uint32_t i = 0; i < proc_num; i++) {
      auto ctx = std::make_shared<ClassicContext>();//????????????????????????????????????????????????????????????????????????
      ctx->SetGroupName(group_name);
      pctxs_.emplace_back(ctx);

      auto proc = std::make_shared<Processor>();// ?????? Processor ??? ClassicContext ?????????????????????,???????????????
      proc->BindContext(ctx);
      proc->SetAffinity(cpuset, affinity, i);
      proc->SetSchedPolicy(processor_policy, processor_prio);//???????????? Processor(??????)???????????????????????????
      processors_.emplace_back(proc);
    }
  }
}
//1 ??????????????????????????? 2 ??????????????????????????????????????????????????? 3 ????????????????????????
bool SchedulerClassic::DispatchTask(const std::shared_ptr<CRoutine>& cr) {
  // we use multi-key mutex to prevent race condition ??????????????????????????????
  // when del && add cr with same crid
  if (likely(id_cr_wl_.find(cr->id()) == id_cr_wl_.end())) {
    {
      std::lock_guard<std::mutex> wl_lg(cr_wl_mtx_);
      if (id_cr_wl_.find(cr->id()) == id_cr_wl_.end()) {
        id_cr_wl_[cr->id()];
      }
    }
  }
  std::lock_guard<std::mutex> lg(id_cr_wl_[cr->id()]);

  {
    WriteLockGuard<AtomicRWLock> lk(id_cr_lock_);
    if (id_cr_.find(cr->id()) != id_cr_.end()) {
      return false;
    }
    id_cr_[cr->id()] = cr;
  }

  if (cr_confs_.find(cr->name()) != cr_confs_.end()) {
    ClassicTask task = cr_confs_[cr->name()];
    cr->set_priority(task.prio());
    cr->set_group_name(task.group_name());
  } else {
    // croutine that not exist in conf
    cr->set_group_name(classic_conf_.groups(0).name()); //???????????????????????? ??????????????????
  }

  // Check if task prio is reasonable.
  if (cr->priority() >= MAX_PRIO) {
    AWARN << cr->name() << " prio is greater than MAX_PRIO[ << " << MAX_PRIO
          << "].";
    cr->set_priority(MAX_PRIO - 1);
  }

  // Enqueue task. ???????????????????????????????????????????????????????????????
  {
    WriteLockGuard<AtomicRWLock> lk(
        ClassicContext::rq_locks_[cr->group_name()].at(cr->priority()));
    ClassicContext::cr_group_[cr->group_name()].at(cr->priority())
        .emplace_back(cr); 
  }

  PerfEventCache::Instance()->AddSchedEvent(SchedPerf::RT_CREATE, cr->id(),
                                            cr->processor_id());
  ClassicContext::Notify(cr->group_name());// ??????
  return true;
}
// ???????????????????????????notify processor ?????????????????????
bool SchedulerClassic::NotifyProcessor(uint64_t crid) {
  if (unlikely(stop_)) {
    return true;
  }

  {
    ReadLockGuard<AtomicRWLock> lk(id_cr_lock_);
    if (id_cr_.find(crid) != id_cr_.end()) {
      auto cr = id_cr_[crid];
      if (cr->state() == RoutineState::DATA_WAIT) {
        cr->SetUpdateFlag(); //??????????????????????????????????????????????????????????????????
      }

      ClassicContext::Notify(cr->group_name());// ????????? Processor::Run ???wait?????????  ?????????notify_one?????????NotifyProcessor????????????????????????tid???
      return true;
    }
  }
  return false;
}

bool SchedulerClassic::RemoveTask(const std::string& name) {
  if (unlikely(stop_)) {
    return true;
  }

  auto crid = GlobalData::GenerateHashId(name);
  return RemoveCRoutine(crid);
}
// ????????????????????????????????????id???????????????????????????????????????
bool SchedulerClassic::RemoveCRoutine(uint64_t crid) {
  // we use multi-key mutex to prevent race condition
  // when del && add cr with same crid
  if (unlikely(id_cr_wl_.find(crid) == id_cr_wl_.end())) {
    {
      std::lock_guard<std::mutex> wl_lg(cr_wl_mtx_);
      if (id_cr_wl_.find(crid) == id_cr_wl_.end()) {
        id_cr_wl_[crid];
      }
    }
  }
  std::lock_guard<std::mutex> lg(id_cr_wl_[crid]);

  std::shared_ptr<CRoutine> cr = nullptr;
  int prio;
  std::string group_name;
  {
    WriteLockGuard<AtomicRWLock> lk(id_cr_lock_);
    if (id_cr_.find(crid) != id_cr_.end()) {
      cr = id_cr_[crid];
      prio = cr->priority();
      group_name = cr->group_name();
      id_cr_[crid]->Stop();
      id_cr_.erase(crid);
    } else {
      return false;
    }
  }

  WriteLockGuard<AtomicRWLock> lk(
      ClassicContext::rq_locks_[group_name].at(prio));
  for (auto it = ClassicContext::cr_group_[group_name].at(prio).begin();
       it != ClassicContext::cr_group_[group_name].at(prio).end(); ++it) {
    if ((*it)->id() == crid) {
      auto cr = *it;

      (*it)->Stop();//?????????????????????state?????????FINISHED
      it = ClassicContext::cr_group_[group_name].at(prio).erase(it);
      cr->Release();
      return true;
    }
  }

  return false;
}

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo
