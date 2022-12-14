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

#ifndef CYBER_SCHEDULER_POLICY_PROCESSOR_CONTEXT_H_
#define CYBER_SCHEDULER_POLICY_PROCESSOR_CONTEXT_H_

#include <limits>
#include <memory>
#include <mutex>

#include "cyber/base/macros.h"
#include "cyber/croutine/croutine.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using croutine::CRoutine;

class Processor;

class ProcessorContext {
 public:
  virtual void Shutdown();
  virtual std::shared_ptr<CRoutine> NextRoutine() = 0;
  virtual void Wait() = 0;

 protected:
  bool stop_ = false;
  alignas(CACHELINE_SIZE) std::atomic_flag notified_ = ATOMIC_FLAG_INIT;//内存对齐至64位
};

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SCHEDULER_PROCESSOR_CONTEXT_H_
