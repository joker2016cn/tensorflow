/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/profiler/utils/group_events.h"

#include <stack>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_join.h"
#include "absl/types/optional.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/profiler/utils/tf_xplane_visitor.h"
#include "tensorflow/core/profiler/utils/xplane_schema.h"
#include "tensorflow/core/profiler/utils/xplane_utils.h"

namespace tensorflow {
namespace profiler {
namespace {

// Returns event type if it is a KernelLaunch or KernelExecute event.
absl::optional<int64> GetKernelEventType(const XPlaneVisitor& visitor,
                                         const XEvent& event) {
  bool found_correlation_id = false;
  bool found_device_id = false;
  for (const auto& stat : event.stats()) {
    if (visitor.GetStatType(stat) == StatType::kCorrelationId) {
      found_correlation_id = true;
    } else if (visitor.GetStatType(stat) == StatType::kDeviceId) {
      found_device_id = true;
    }
  }
  if (found_correlation_id) {
    return found_device_id ? HostEventType::kKernelLaunch
                           : HostEventType::kKernelExecute;
  }
  return absl::nullopt;
}

const XStat* GetStat(const XPlaneVisitor& visitor, const XEvent& event,
                     int64 stat_type) {
  for (const auto& stat : event.stats()) {
    if (visitor.GetStatType(stat) == stat_type) {
      return &stat;
    }
  }
  return nullptr;
}

void SetGroupId(const XPlaneVisitor& visitor, int64 group_id, XEvent* event) {
  absl::optional<int64> maybe_group_id_stat_metadata_id =
      visitor.GetStatMetadataId(StatType::kGroupId);
  // TODO(jihochoi): Create stat metadata for group_id if not found.
  if (maybe_group_id_stat_metadata_id) {
    AddOrUpdateIntStat(*maybe_group_id_stat_metadata_id, group_id, event);
  }
}

// Create EventNodeMap with the event types in connect_info_list and
// root_event_types.
EventNodeMap CreateEventNodeMap(
    const std::vector<InterThreadConnectInfo>& connect_info_list,
    const std::vector<int64>& root_event_types) {
  EventNodeMap event_node_map;
  for (const auto& connect_info : connect_info_list) {
    event_node_map.try_emplace(connect_info.parent_event_type);
    event_node_map.try_emplace(connect_info.child_event_type);
  }
  for (int64 event_type : root_event_types) {
    event_node_map.try_emplace(event_type);
  }
  return event_node_map;
}

}  // namespace

absl::optional<const XStat*> EventNode::GetContextStat(int64 stat_type) const {
  if (const XStat* stat = GetStat(*visitor_, *event_, stat_type)) {
    return stat;
  } else if (parent_) {
    return parent_->GetContextStat(stat_type);
  }
  return absl::nullopt;
}

std::string EventNode::GetGroupName() const {
  std::vector<std::string> name_parts;
  if (auto graph_type_stat = GetContextStat(StatType::kGraphType);
      graph_type_stat.has_value()) {
    name_parts.push_back((*graph_type_stat)->str_value());
  }
  int64 step_num = 0;
  if (auto step_num_stat = GetContextStat(StatType::kStepNum);
      step_num_stat.has_value()) {
    step_num = (*step_num_stat)->int64_value();
  }
  if (auto iter_num_stat = GetContextStat(StatType::kIterNum);
      iter_num_stat.has_value()) {
    step_num += (*iter_num_stat)->int64_value();
  }
  name_parts.push_back(absl::StrCat(step_num));
  return absl::StrJoin(name_parts, " ");
}

void EventNode::PropagateGroupId(int64 group_id) {
  group_id_ = group_id;
  SetGroupId(*visitor_, group_id, event_);
  for (const auto& child : children_) {
    child->PropagateGroupId(*group_id_);
  }
}

void EventNode::AddStepName(absl::string_view step_name) {
  AddOrUpdateStrStat(*visitor_->GetStatMetadataId(StatType::kStepName),
                     step_name, event_);
}

void ConnectIntraThread(const XPlaneVisitor& visitor, XPlane* host_trace,
                        EventNodeMap* event_node_map) {
  for (auto& line : *host_trace->mutable_lines()) {
    // absl::string_view thread_name = line.name();
    std::stack<std::shared_ptr<EventNode>> parent_nodes;
    for (auto& event : *line.mutable_events()) {
      auto cur_node = std::make_shared<EventNode>(&visitor, &event);
      while (!parent_nodes.empty()) {
        std::shared_ptr<EventNode> parent_node = parent_nodes.top();
        if (IsNested(cur_node->GetEvent(), parent_node->GetEvent())) {
          parent_node->AddChild(cur_node);
          cur_node->SetParent(parent_node.get());
          break;
        } else {
          parent_nodes.pop();
        }
      }
      parent_nodes.push(cur_node);
      if (auto it = gtl::FindOrNull(
              *event_node_map, visitor.GetEventType(event).value_or(
                                   HostEventType::kUnknownHostEventType))) {
        it->push_back(cur_node);
      }
      // KernelLaunch and KernelExecute event types are not supported by
      // XPlaneVisitor and should be checked separately.
      // TODO(148346217): Make XPlaneVisitor support KernelLaunch and
      // KernelExecute event types.
      if (event_node_map->contains(HostEventType::kKernelLaunch) ||
          event_node_map->contains(HostEventType::kKernelExecute)) {
        absl::optional<int64> kernel_event_type =
            GetKernelEventType(visitor, event);
        if (kernel_event_type) {
          (*event_node_map)[*kernel_event_type].push_back(cur_node);
        }
      }
    }
  }
}

void ConnectInterThread(
    const EventNodeMap& event_node_map,
    const std::vector<InterThreadConnectInfo>& connect_info_list) {
  for (const auto& connect_info : connect_info_list) {
    absl::flat_hash_map<std::vector<int64>, std::shared_ptr<EventNode>>
        connect_map;
    const std::vector<int64>& stat_types = connect_info.stat_types;
    if (auto parent_event_node_list =
            gtl::FindOrNull(event_node_map, connect_info.parent_event_type)) {
      for (const auto& parent_event_node : *parent_event_node_list) {
        std::vector<int64> stats;
        for (auto stat_type : stat_types) {
          absl::optional<const XStat*> stat =
              parent_event_node->GetContextStat(stat_type);
          if (!stat) break;
          stats.push_back((*stat)->value_case() == (*stat)->kInt64Value
                              ? (*stat)->int64_value()
                              : (*stat)->uint64_value());
        }
        if (stats.size() == stat_types.size()) {
          connect_map[stats] = parent_event_node;
        }
      }
    }
    if (auto child_event_node_list =
            gtl::FindOrNull(event_node_map, connect_info.child_event_type)) {
      for (const auto& child_event_node : *child_event_node_list) {
        std::vector<int64> stats;
        for (auto stat_type : stat_types) {
          absl::optional<const XStat*> stat =
              child_event_node->GetContextStat(stat_type);
          if (!stat) break;
          stats.push_back((*stat)->value_case() == (*stat)->kInt64Value
                              ? (*stat)->int64_value()
                              : (*stat)->uint64_value());
        }
        if (stats.size() == stat_types.size()) {
          if (auto parent_event_node = gtl::FindOrNull(connect_map, stats)) {
            (*parent_event_node)->AddChild(child_event_node);
            child_event_node->SetParent((*parent_event_node).get());
          }
        }
      }
    }
  }
}

void CreateEventGroup(const std::vector<int64 /*EventType*/>& root_event_types,
                      const EventNodeMap& event_node_map,
                      EventGroupNameMap* event_group_name_map) {
  int64 next_group_id = 0;
  for (int64 root_event_type : root_event_types) {
    if (auto root_event_node_list =
            gtl::FindOrNull(event_node_map, root_event_type)) {
      for (const auto& root_event_node : *root_event_node_list) {
        // Skip if it already belongs to a group.
        if (root_event_node->GetGroupId()) continue;
        int64 group_id = next_group_id++;
        root_event_node->PropagateGroupId(group_id);
        (*event_group_name_map)[group_id] = root_event_node->GetGroupName();
        // Add step_name stat if it is a TraceContext event.
        // TODO(jihochoi): change event name instead.
        if (root_event_type == HostEventType::kTraceContext) {
          root_event_node->AddStepName((*event_group_name_map)[group_id]);
        }
      }
    }
  }
}

void GroupEvents(const std::vector<InterThreadConnectInfo>& connect_info_list,
                 const std::vector<int64>& root_event_types, XPlane* host_trace,
                 const std::vector<XPlane*>& device_traces,
                 EventGroupNameMap* event_group_name_map) {
  if (!host_trace) return;
  EventNodeMap event_node_map =
      CreateEventNodeMap(connect_info_list, root_event_types);
  XPlaneVisitor host_plane_visitor = CreateTfXPlaneVisitor(host_trace);
  ConnectIntraThread(host_plane_visitor, host_trace, &event_node_map);
  std::vector<XPlaneVisitor> device_plane_visitors;
  device_plane_visitors.reserve(device_traces.size());
  for (XPlane* device_trace : device_traces) {
    device_plane_visitors.push_back(CreateTfXPlaneVisitor(device_trace));
    ConnectIntraThread(device_plane_visitors.back(), device_trace,
                       &event_node_map);
  }
  ConnectInterThread(event_node_map, connect_info_list);
  CreateEventGroup(root_event_types, event_node_map, event_group_name_map);
}

void GroupTfEvents(XPlane* host_trace,
                   const std::vector<XPlane*>& device_traces,
                   EventGroupNameMap* event_group_name_map) {
  std::vector<InterThreadConnectInfo> connect_info_list(
      {{HostEventType::kFunctionRun,
        HostEventType::kExecutorStateProcess,
        {StatType::kStepId}},
       {HostEventType::kSessionRun,
        HostEventType::kExecutorStateProcess,
        {StatType::kStepId}},
       {HostEventType::kKernelLaunch,
        HostEventType::kKernelExecute,
        {StatType::kCorrelationId}}});
  const std::vector<int64 /*EventType*/> root_event_types(
      {HostEventType::kTraceContext, HostEventType::kFunctionRun,
       HostEventType::kSessionRun});
  GroupEvents(connect_info_list, root_event_types, host_trace, device_traces,
              event_group_name_map);
}

}  // namespace profiler
}  // namespace tensorflow
