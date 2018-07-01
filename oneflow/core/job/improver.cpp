#include "oneflow/core/job/improver.h"
#include "oneflow/core/graph/task_node.h"
#include "oneflow/core/register/register_desc.pb.h"
#include "oneflow/core/register/register_manager.h"
#include "oneflow/core/job/job_desc.h"
#include "oneflow/core/job/profiler.h"
#include "oneflow/core/graph/plan_task_graph.h"
#include "oneflow/core/graph/regst_lifetime_graph.h"
#include "oneflow/core/actor/act_event_logger.h"

namespace oneflow {

namespace {

bool IsSharableRegstWithoutConsumer(const RegstDescProto& regst_desc) {
  return regst_desc.consumer_task_id_size() == 0 && regst_desc.enable_mem_sharing();
}

bool IsConsumersAndProducerInSameChain(const RegstDescProto& regst_desc,
                                       const std::function<int64_t(int64_t)>& ChainId4TaskId) {
  int64_t producer_chain_id = ChainId4TaskId(regst_desc.producer_task_id());
  for (int64_t consumer_task_id : regst_desc.consumer_task_id()) {
    if (ChainId4TaskId(consumer_task_id) != producer_chain_id) { return false; }
  }
  return true;
}

bool IsSharableRegstWithConsumer(const RegstDescProto& regst_desc,
                                 const std::function<int64_t(int64_t)>& ChainId4TaskId) {
  return regst_desc.consumer_task_id_size() > 0 && regst_desc.enable_mem_sharing()
         && regst_desc.register_num() == 1
         && IsConsumersAndProducerInSameChain(regst_desc, ChainId4TaskId);
}

void ForEachSharableStreamRegstDescsWithoutConsumer(
    const Plan& plan, const std::function<void(const std::list<const RegstDescProto*>&)>& Handler) {
  HashMap<int64_t, std::list<const RegstDescProto*>> global_work_stream_id2regst_descs;
  for (const auto& task : plan.task()) {
    int64_t global_work_stream_id = Global<IDMgr>::Get()->GlobalWorkStreamId4TaskId(task.task_id());
    for (const auto& pair : task.produced_regst_desc()) {
      if (IsSharableRegstWithoutConsumer(pair.second)) {
        global_work_stream_id2regst_descs[global_work_stream_id].push_back(&pair.second);
      }
    }
  }
  for (const auto& pair : global_work_stream_id2regst_descs) {
    if (pair.second.size() > 1) { Handler(pair.second); }
  }
}

void ForEachSharableChainRegstDescsWithConsumer(
    const Plan& plan, const std::function<int64_t(int64_t)>& ChainId4TaskId,
    const std::function<void(const std::list<const RegstDescProto*>&)>& Handler) {
  HashMap<int64_t, std::list<const TaskProto*>> chain_id2task_proto;
  for (const TaskProto& task : plan.task()) {
    chain_id2task_proto[task.task_set_info().chain_id()].push_back(&task);
  }
  for (const auto& chain_tasks_pair : chain_id2task_proto) {
    if (chain_tasks_pair.second.size() == 1) { continue; }
    std::list<const RegstDescProto*> regst_descs;
    for (const TaskProto* task : chain_tasks_pair.second) {
      for (const auto& pair : task->produced_regst_desc()) {
        if (IsSharableRegstWithConsumer(pair.second, ChainId4TaskId)) {
          regst_descs.push_back(&pair.second);
        }
      }
    }
    if (regst_descs.size() > 1) { Handler(regst_descs); }
  }
}

void ForEachSameColoredStreamRegstDescWithoutConsumer(
    const Plan& plan, const std::function<void(const std::list<const RegstDescProto*>&)>& Handler) {
  auto GetProducerTaskId = [](const RegstDescProto* regst_desc, HashSet<int64_t>* ret_actor_ids) {
    CHECK(regst_desc->enable_mem_sharing());
    ret_actor_ids->insert(regst_desc->producer_task_id());
  };
  ForEachSharableStreamRegstDescsWithoutConsumer(
      plan, [&](const std::list<const RegstDescProto*>& regst_descs) {
        RegstLifetimeGraph(regst_descs, GetProducerTaskId).ForEachSameColoredRegstDescs(Handler);
      });
}

void ForEachSameColoredChainRegstDescWithConsumer(
    const PlanTaskGraph& plan_task_graph,
    const std::function<void(const std::list<const RegstDescProto*>&)>& Handler) {
  auto ComputeLifetimeSameChainActorIds = [&](const RegstDescProto* regst_desc,
                                              HashSet<int64_t>* ret_actor_ids) {
    CHECK(regst_desc->enable_mem_sharing());
    ret_actor_ids->clear();
    plan_task_graph.ComputeLifetimeSameChainActorIds(regst_desc, ret_actor_ids);
  };
  auto ChainId4TaskId = [&](int64_t task_id) {
    return plan_task_graph.TaskProto4TaskId(task_id)->task_set_info().chain_id();
  };
  const Plan& plan = plan_task_graph.plan();
  ForEachSharableChainRegstDescsWithConsumer(
      plan, ChainId4TaskId, [&](const std::list<const RegstDescProto*>& regst_descs) {
        RegstLifetimeGraph(regst_descs, ComputeLifetimeSameChainActorIds)
            .ForEachSameColoredRegstDescs(Handler);
      });
}

void ForEachImprovedMemSharedId(const PlanTaskGraph& plan_task_graph,
                                const std::function<void(int64_t, int64_t)>& Handler) {
  using RegstDescs = std::list<const RegstDescProto*>;
  const Plan& plan = plan_task_graph.plan();
  auto HandleMemSharedId = [&](const RegstDescs& regst_descs) {
    int64_t mem_shared_id = Global<IDMgr>::Get()->NewMemSharedId();
    for (const RegstDescProto* regst_desc : regst_descs) {
      Handler(regst_desc->regst_desc_id(), mem_shared_id);
    }
  };
  ForEachSameColoredStreamRegstDescWithoutConsumer(plan, HandleMemSharedId);
  ForEachSameColoredChainRegstDescWithConsumer(plan_task_graph, HandleMemSharedId);
}

double CalcRegstNum(double regst_desc_duration, double ii, double ii_scale) {
  return ((ii_scale - 1) * ii + regst_desc_duration) / (ii_scale * ii);
}

double CalcII(double regst_desc_duration, uint64_t regst_num, double ii_scale) {
  return regst_desc_duration / ((regst_num - 1) * ii_scale + 1);
}

uint64_t CalcRegstNum(
    const RegstDescProto& regst_desc,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathDurations4RegstDescId,
    double ii,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathIIScales4RegstDescId) {
  int64_t regst_desc_id = regst_desc.regst_desc_id();
  const auto& consumer_actor_id2duration = PathDurations4RegstDescId(regst_desc_id);
  const auto& consumer_actor_id2ii_scale = PathIIScales4RegstDescId(regst_desc_id);
  uint64_t regst_num = 0;
  for (const auto& pair : consumer_actor_id2duration) {
    double duration = pair.second;
    double ii_scale = consumer_actor_id2ii_scale.at(pair.first);
    uint64_t cur_path_regst_num = ceil(CalcRegstNum(duration, ii, ii_scale));
    regst_num = std::max(regst_num, cur_path_regst_num);
  }
  regst_num = std::max(regst_num, static_cast<uint64_t>(regst_desc.min_register_num()));
  regst_num = std::min(regst_num, static_cast<uint64_t>(regst_desc.max_register_num()));
  return regst_num;
}

uint64_t CalcMemoryConsumed(
    const std::list<const RegstDescProto*>& regst_descs,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathDurations4RegstDescId,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathIIScales4RegstDescId,
    double ii) {
  uint64_t mem_consuming = 0;
  HashMap<int64_t, uint64_t> mem_shared_id2max_regst_desc_mem_bytes;
  for (const RegstDescProto* regst_desc : regst_descs) {
    uint64_t regst_num =
        CalcRegstNum(*regst_desc, PathDurations4RegstDescId, ii, PathIIScales4RegstDescId);
    uint64_t total_byte_size = RtRegstDesc(*regst_desc).packed_blob_desc()->TotalByteSize();
    if (regst_desc->mem_shared_id() == -1) {
      mem_consuming += regst_num * total_byte_size;
    } else {
      CHECK_EQ(regst_num, 1);
      int32_t mem_shared_id = regst_desc->mem_shared_id();
      auto& max_bytes = mem_shared_id2max_regst_desc_mem_bytes[mem_shared_id];
      max_bytes = std::max(max_bytes, total_byte_size);
    }
  }
  for (const auto& pair : mem_shared_id2max_regst_desc_mem_bytes) { mem_consuming += pair.second; }
  return mem_consuming;
}

std::shared_ptr<HashMap<int64_t, RegstDescProto*>> MakeRegstDescId2RegstDesc(Plan* plan) {
  auto regst_desc_id2regst_desc = std::make_shared<HashMap<int64_t, RegstDescProto*>>();
  for (int i = 0; i < plan->task_size(); i++) {
    TaskProto* task = plan->mutable_task(i);
    for (auto& pair : *task->mutable_produced_regst_desc()) {
      int64_t regst_desc_id = pair.second.regst_desc_id();
      regst_desc_id2regst_desc->insert({regst_desc_id, &pair.second});
    }
  }
  return regst_desc_id2regst_desc;
}

std::function<void(int64_t, uint64_t)> MakeSetterSetPlanRegstNum(Plan* plan) {
  auto regst_desc_id2regst_desc = MakeRegstDescId2RegstDesc(plan);
  return [regst_desc_id2regst_desc](int64_t regst_desc_id, uint64_t num) {
    regst_desc_id2regst_desc->at(regst_desc_id)->set_register_num(num);
  };
}

std::function<void(int64_t, int64_t)> MakeSetterSetPlanMemSharedId(Plan* plan) {
  auto regst_desc_id2regst_desc = MakeRegstDescId2RegstDesc(plan);
  return [regst_desc_id2regst_desc](int64_t regst_desc_id, int64_t mem_shared_id) {
    regst_desc_id2regst_desc->at(regst_desc_id)->set_mem_shared_id(mem_shared_id);
  };
}

std::function<const HashMap<int64_t, double>&(int64_t)> MakeGetterPathDurations4RegstDescId(
    const ActGraph& graph) {
  auto regst_desc_id2consumer_id2duration =
      std::make_shared<HashMap<int64_t, HashMap<int64_t, double>>>();
  graph.ForEachRegstDescConsumerPathMeanDuration(
      [&](int64_t regst_desc_id, int64_t consumer_actor_id, double time) {
        (*regst_desc_id2consumer_id2duration)[regst_desc_id][consumer_actor_id] = time;
      });
  auto empty = std::make_shared<const HashMap<int64_t, double>>();
  return [regst_desc_id2consumer_id2duration,
          empty](int64_t regst_desc_id) -> const HashMap<int64_t, double>& {
    const auto& it = regst_desc_id2consumer_id2duration->find(regst_desc_id);
    if (it == regst_desc_id2consumer_id2duration->end()) {
      return *empty;
    } else {
      return it->second;
    }
  };
}

uint64_t NumOfPiecesInSnapshot() {
  return Global<JobDesc>::Get()->NumOfBatchesInSnapshot()
         * Global<JobDesc>::Get()->NumOfPiecesInBatch();
}

double FormalDuration4ExperimentalDuration(TaskType task_type, double duration,
                                           double act_frequency) {
  if (task_type == TaskType::kMdSave) {
    double formal_run_frequency = 1.0 / NumOfPiecesInSnapshot();
    return (duration / act_frequency) * formal_run_frequency;
  }
  return duration;
}

double CalcBaseII(const ActGraph& act_graph) {
  int64_t max_act_cnt = 0;
  for (const auto& pair : act_graph.actor_id2act_cnt()) {
    if (max_act_cnt < pair.second) { max_act_cnt = pair.second; }
  }
  HashMap<int64_t, double> actor_id2act_frequency;
  for (const auto& pair : act_graph.actor_id2act_cnt()) {
    actor_id2act_frequency[pair.first] = 1.0 * pair.second / max_act_cnt;
  }
  HashMap<int64_t, double> stream_id2total_calc_time;
  act_graph.ForEachNode([&](const ActNode* act_node) {
    int64_t stream_id = act_node->act_event().work_stream_id();
    int64_t actor_id = act_node->actor_id();
    TaskType task_type = act_graph.GetTaskProto(actor_id).task_type();
    stream_id2total_calc_time[stream_id] += FormalDuration4ExperimentalDuration(
        task_type, act_node->Duration(), actor_id2act_frequency.at(actor_id));
  });
  double base_ii = 0;
  for (const auto& pair : stream_id2total_calc_time) {
    base_ii = std::max(base_ii, pair.second / max_act_cnt);
  }
  return base_ii;
}

double IIScale4Actor(TaskType task_type, double default_ii_scale) {
  if (task_type == TaskType::kMdSave) { return NumOfPiecesInSnapshot(); }
  return default_ii_scale;
}

std::function<const HashMap<int64_t, double>&(int64_t)> MakeGetterPathIIScales4RegstDescId(
    const ActGraph& graph) {
  auto regst_desc_id2consumer_id2ii_scale =
      std::make_shared<HashMap<int64_t, HashMap<int64_t, double>>>();
  graph.ForEachRegstDescConsumerPathIIScale(
      [&](int64_t regst_desc_id, int64_t consumer_actor_id, double ii_scale) {
        TaskType task_type = graph.GetTaskProto(consumer_actor_id).task_type();
        (*regst_desc_id2consumer_id2ii_scale)[regst_desc_id][consumer_actor_id] =
            IIScale4Actor(task_type, ii_scale);
      });
  auto empty = std::make_shared<const HashMap<int64_t, double>>();
  return [regst_desc_id2consumer_id2ii_scale,
          empty](int64_t regst_desc_id) -> const HashMap<int64_t, double>& {
    const auto& it = regst_desc_id2consumer_id2ii_scale->find(regst_desc_id);
    if (it == regst_desc_id2consumer_id2ii_scale->end()) {
      return *empty;
    } else {
      return it->second;
    }
  };
}

void TryConnectWithMemSafeGuardCtrlRegstDesc(TaskProto* src_task_proto, TaskProto* dst_task_proto) {
  RegstDescProto* ctrl_regst_desc =
      FindOrCreateProducedCtrlRegstDesc(src_task_proto, "out_ctrl_shared_mem_safe_guard");
  int64_t dst_task_id = dst_task_proto->task_id();
  if (!IsInRepeatedField(ctrl_regst_desc->consumer_task_id(), dst_task_id)) {
    ctrl_regst_desc->add_consumer_task_id(dst_task_id);
    int64_t ctrl_regst_desc_id = ctrl_regst_desc->regst_desc_id();
    RegstDescIdSet* consumed_ctrl_regst_desc_ids =
        FindOrCreateConsumedCtrlRegstDescIdSet(dst_task_proto, "in_ctrl");
    CHECK(!IsInRepeatedField(consumed_ctrl_regst_desc_ids->regst_desc_id(), ctrl_regst_desc_id));
    consumed_ctrl_regst_desc_ids->add_regst_desc_id(ctrl_regst_desc_id);
  }
}

void CollectTailRegstConsumerTaskIds(const std::vector<const RegstDescProto*>& shared_mem_regsts,
                                     HashSet<int64_t>* task_ids) {
  for (const RegstDescProto* regst_proto : shared_mem_regsts) {
    if (regst_proto == shared_mem_regsts.front()) { continue; }
    for (int64_t consumer_id : regst_proto->consumer_task_id()) { task_ids->insert(consumer_id); }
  }
}

void CollectSinkTaskIds(const HashSet<int64_t>& task_ids,
                        const std::function<bool(int64_t, int64_t)>& IsReachable,
                        std::list<int64_t>* sink_task_ids) {
  auto IsReachableToAnyOherTask = [&](int64_t src_task_id) -> bool {
    for (int64_t dst_task_id : task_ids) {
      if (src_task_id == dst_task_id) { continue; }
      if (IsReachable(src_task_id, dst_task_id)) { return true; }
    }
    return false;
  };
  sink_task_ids->clear();
  for (int64_t src_task_id : task_ids) {
    if (!IsReachableToAnyOherTask(src_task_id)) { sink_task_ids->push_back(src_task_id); }
  }
}

std::function<void(const std::vector<const RegstDescProto*>&)> MakeSetterAddCtrlRegst(
    Plan* plan, const std::function<bool(int64_t, int64_t)>& IsReachable) {
  auto task_id2task_proto = std::make_shared<HashMap<int64_t, TaskProto*>>();
  for (int i = 0; i < plan->task_size(); i++) {
    TaskProto* task_proto = plan->mutable_task(i);
    CHECK(task_id2task_proto->emplace(task_proto->task_id(), task_proto).second);
  }
  return [task_id2task_proto,
          IsReachable](const std::vector<const RegstDescProto*>& shared_mem_regsts) {
    if (shared_mem_regsts.size() == 1) { return; }
    int64_t header_task_id = shared_mem_regsts.front()->producer_task_id();
    TaskProto* header_task_proto = task_id2task_proto->at(header_task_id);
    HashSet<int64_t> tail_regsts_consumer_task_ids;
    CollectTailRegstConsumerTaskIds(shared_mem_regsts, &tail_regsts_consumer_task_ids);
    std::list<int64_t> sink_task_ids;
    CollectSinkTaskIds(tail_regsts_consumer_task_ids, IsReachable, &sink_task_ids);
    for (int64_t sink_task_id : sink_task_ids) {
      TaskProto* sink_task_proto = task_id2task_proto->at(sink_task_id);
      TryConnectWithMemSafeGuardCtrlRegstDesc(header_task_proto, sink_task_proto);
    }
  };
}

void ForEachMemSharingCriticalSection(
    const Plan& plan, const std::function<int64_t(int64_t)>& OrderInGraph4TaskId,
    const std::function<void(const std::vector<const RegstDescProto*>&)>& Handler) {
  HashMap<int32_t, std::vector<const RegstDescProto*>> mem_sharing_id2regst_descs;
  for (const auto& task : plan.task()) {
    for (const auto& pair : task.produced_regst_desc()) {
      int32_t mem_sharing_id = pair.second.mem_shared_id();
      if (mem_sharing_id != -1 && pair.second.consumer_task_id_size() > 0) {
        CHECK(pair.second.enable_mem_sharing());
        mem_sharing_id2regst_descs[mem_sharing_id].push_back(&pair.second);
      }
    }
  }
  for (auto& pair : mem_sharing_id2regst_descs) {
    std::sort(pair.second.begin(), pair.second.end(),
              [&](const RegstDescProto* lhs, const RegstDescProto* rhs) {
                int64_t lhs_order_in_graph = OrderInGraph4TaskId(lhs->producer_task_id());
                int64_t rhs_order_in_graph = OrderInGraph4TaskId(rhs->producer_task_id());
                CHECK_NE(lhs_order_in_graph, rhs_order_in_graph);
                return lhs_order_in_graph < rhs_order_in_graph;
              });
    Handler(pair.second);
  }
}

}  // namespace

uint64_t Improver::AvailableMemSize(int64_t machine_id, int64_t memory_zone_id) const {
  int64_t mem_size = amd_.machine_amd(machine_id).zone_size(memory_zone_id);
  JobDesc* job_desc = Global<JobDesc>::Get();
  if (memory_zone_id == job_desc->GpuDeviceNum()) {
    mem_size -= job_desc->reserved_host_mem_byte();
    mem_size -= job_desc->persistence_buf_byte() * record_load_task_num_.at(machine_id);
  } else {
    mem_size -= job_desc->reserved_device_mem_byte();
  }
  CHECK_GT(mem_size, 0);
  return static_cast<uint64_t>(mem_size);
}

int64_t Improver::GetMemoryZoneId(const MemoryCase& mem_case) const {
  if (mem_case.has_device_cuda_mem()) {
    return mem_case.device_cuda_mem().device_id();
  } else {
    return Global<JobDesc>::Get()->GpuDeviceNum();
  }
}

void Improver::MakeMemZoneRegstDescs(const Plan& plan, MemZoneRegstDescs* mz2regst_desc) const {
  mz2regst_desc->resize(amd_.machine_amd_size());
  FOR_RANGE(int64_t, machine_id, 0, amd_.machine_amd_size()) {
    mz2regst_desc->at(machine_id).resize(amd_.machine_amd(machine_id).zone_size_size());
  }
  for (const auto& task : plan.task()) {
    for (const auto& pair : task.produced_regst_desc()) {
      int64_t mem_zone_id = GetMemoryZoneId(pair.second.mem_case());
      mz2regst_desc->at(task.machine_id()).at(mem_zone_id).push_back(&pair.second);
    }
  }
}

bool Improver::IsAnyZoneOutOfMemory(
    const MemZoneRegstDescs& mz_regst_descs,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathDurations4RegstDescId,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathIIScales4RegstDescId,
    double ii) const {
  FOR_RANGE(int64_t, machine_id, 0, mz_regst_descs.size()) {
    FOR_RANGE(int64_t, mem_zone_id, 0, mz_regst_descs[machine_id].size()) {
      const auto& regst_descs = mz_regst_descs[machine_id][mem_zone_id];
      if (CalcMemoryConsumed(regst_descs, PathDurations4RegstDescId, PathIIScales4RegstDescId, ii)
          >= AvailableMemSize(machine_id, mem_zone_id)) {
        return true;
      }
    }
  }
  return false;
}

double Improver::CalcMaxRegstDescDuration(
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathDurations4RegstDescId,
    const MemZoneRegstDescs& mz_regst_descs) const {
  double max_duration = 0;
  for (const auto& zone_regst_descs : mz_regst_descs) {
    for (const auto& regst_descs : zone_regst_descs) {
      for (const RegstDescProto* regst_desc : regst_descs) {
        for (const auto& pair : PathDurations4RegstDescId(regst_desc->regst_desc_id())) {
          max_duration = std::max(max_duration, pair.second);
        }
      }
    }
  }
  return max_duration;
}

double Improver::BinarySearchII(
    double base_ii,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathDurations4RegstDescId,
    const std::function<const HashMap<int64_t, double>&(int64_t)>& PathIIScales4RegstDescId,
    const MemZoneRegstDescs& mz_regst_descs) const {
  double max_duration = CalcMaxRegstDescDuration(PathDurations4RegstDescId, mz_regst_descs);
  CHECK(!IsAnyZoneOutOfMemory(mz_regst_descs, PathDurations4RegstDescId, PathIIScales4RegstDescId,
                              max_duration));
  const double ii_search_threshold = 1;
  double r = max_duration;
  double l = base_ii;
  double mid = base_ii;
  while ((r - l) > ii_search_threshold) {
    mid = (l + r) / 2;
    if (IsAnyZoneOutOfMemory(mz_regst_descs, PathDurations4RegstDescId, PathIIScales4RegstDescId,
                             mid)) {
      l = mid;
    } else {
      r = mid;
    }
  }
  return r;
}

void Improver::ForEachImprovedRegstNum(
    const ActGraph& graph, const Plan& plan, bool is_memory_limited,
    const std::function<void(int64_t, uint64_t)>& Handler) const {
  auto PathDurations4RegstDescId = MakeGetterPathDurations4RegstDescId(graph);
  auto PathIIScales4RegstDescId = MakeGetterPathIIScales4RegstDescId(graph);
  double ii = CalcBaseII(graph);
  if (is_memory_limited) {
    MemZoneRegstDescs mz_regst_descs;
    MakeMemZoneRegstDescs(plan, &mz_regst_descs);
    ii = BinarySearchII(ii, PathDurations4RegstDescId, PathIIScales4RegstDescId, mz_regst_descs);
  }
  LOG(INFO) << "memory " << (is_memory_limited ? "limited" : "unlimited") << " ii: " << ii;
  for (const auto& task_proto : plan.task()) {
    for (const auto& pair : task_proto.produced_regst_desc()) {
      uint64_t regst_num =
          CalcRegstNum(pair.second, PathDurations4RegstDescId, ii, PathIIScales4RegstDescId);
      Handler(pair.second.regst_desc_id(), regst_num);
    }
  }
}

Plan Improver::Improve(const AvailableMemDesc& amd, const Plan& naive_plan,
                       const std::string& act_event_filepath) {
  amd_ = amd;
  record_load_task_num_.assign(Global<JobDesc>::Get()->TotalMachineNum(), 0);
  for (const TaskProto& task_proto : naive_plan.task()) {
    if (task_proto.task_type() == TaskType::kRecordLoad) {
      record_load_task_num_.at(Global<IDMgr>::Get()->MachineId4ActorId(task_proto.task_id())) += 1;
    }
  }
  auto act_events = std::make_unique<std::list<ActEvent>>();
  ParseActEvents(act_event_filepath, act_events.get());
  ActGraph act_graph(naive_plan, std::move(act_events));
  Plan mem_unlimited_plan(naive_plan);
  ForEachImprovedRegstNum(act_graph, naive_plan, false,
                          MakeSetterSetPlanRegstNum(&mem_unlimited_plan));
  Plan mem_shared_plan = ImproveMemSharedIdOnly(mem_unlimited_plan);
  Plan plan(mem_shared_plan);
  ForEachImprovedRegstNum(act_graph, mem_shared_plan, true, MakeSetterSetPlanRegstNum(&plan));
  return plan;
}

Plan Improver::ImproveMemSharedIdOnly(const Plan& naive_plan) const {
  Plan plan(naive_plan);
  PlanTaskGraph plan_task_graph(naive_plan);
  ForEachImprovedMemSharedId(plan_task_graph, MakeSetterSetPlanMemSharedId(&plan));
  auto OrderInGraph4TaskId = [&](int64_t task_id) {
    return plan_task_graph.TaskProto4TaskId(task_id)->task_set_info().order_in_graph();
  };
  auto IsReachable = [&](int64_t src_task_id, int64_t dst_task_id) {
    return plan_task_graph.IsReachableInSameArea(src_task_id, dst_task_id);
  };
  ForEachMemSharingCriticalSection(plan, OrderInGraph4TaskId,
                                   MakeSetterAddCtrlRegst(&plan, IsReachable));
  return plan;
}

}  // namespace oneflow
