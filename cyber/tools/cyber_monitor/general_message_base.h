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

#ifndef TOOLS_CVT_MONITOR_GENERAL_MESSAGE_BASE_H_
#define TOOLS_CVT_MONITOR_GENERAL_MESSAGE_BASE_H_

#include <map>
#include <utility>

#include "./renderable_message.h"
#include "cyber/cyber.h"

class Screen;

class GeneralMessageBase : public RenderableMessage {
 protected:

  static void PrintMessage(GeneralMessageBase* baseMsg,
                           const google::protobuf::Message& msg, int& jumpLines,
                           const Screen* s, unsigned& lineNo, int indent);
  static void PrintField(GeneralMessageBase* baseMsg,
                         const google::protobuf::Message& msg, int& jumpLines,
                         const Screen* s, unsigned& lineNo, int indent,
                         const google::protobuf::Reflection* ref,
                         const google::protobuf::FieldDescriptor* field,
                         int index);

  static int lineCount(const google::protobuf::Message& msg, int screenWidth);
  static int lineCountOfField(const google::protobuf::Message& msg,
                              int screenWidth,
                              const google::protobuf::FieldDescriptor* field,
                              const google::protobuf::Reflection* reflection,
                              bool is_folded = true);

  void insertRepeatedMessage(int lineNo, GeneralMessageBase* item) {
    children_map_.insert(std::make_pair(lineNo, item));
  }

  RenderableMessage* Child(int lineNo) const override;

  explicit GeneralMessageBase(RenderableMessage* parent = nullptr)
      : RenderableMessage(parent), children_map_() {}
  ~GeneralMessageBase(void) { clear(); }

  void clear(void) {
    for (auto& iter : children_map_) {
      delete iter.second;
    }

    children_map_.clear();
  }

  GeneralMessageBase(const GeneralMessageBase&) = delete;
  GeneralMessageBase& operator=(const GeneralMessageBase&) = delete;

  std::map<const int, GeneralMessageBase*> children_map_; // lineno : message 树状结构
};

#endif  // TOOLS_CVT_MONITOR_GENERAL_MESSAGE_BASE_H_
