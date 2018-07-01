#ifndef ONEFLOW_CORE_GRAPH_REGST_LIFETIME_GRAPH_H_
#define ONEFLOW_CORE_GRAPH_REGST_LIFETIME_GRAPH_H_

#include "oneflow/core/graph/graph.h"
#include "oneflow/core/register/register_desc.pb.h"

namespace oneflow {

class RegstLifetimeNode;

class RegstLifetimeEdge final : public Edge<RegstLifetimeNode, RegstLifetimeEdge> {
 public:
  OF_DISALLOW_COPY_AND_MOVE(RegstLifetimeEdge);
  RegstLifetimeEdge() = default;
  ~RegstLifetimeEdge() = default;
};

class RegstLifetimeNode final : public Node<RegstLifetimeNode, RegstLifetimeEdge> {
 public:
  OF_DISALLOW_COPY_AND_MOVE(RegstLifetimeNode);
  RegstLifetimeNode(const RegstDescProto* regst_desc,
                    std::unique_ptr<HashSet<int64_t>>&& lifetime_actor_ids)
      : regst_desc_(regst_desc), lifetime_actor_ids_(std::move(lifetime_actor_ids)) {}
  ~RegstLifetimeNode() = default;

  int64_t regst_desc_id() const { return regst_desc().regst_desc_id(); }
  const RegstDescProto& regst_desc() const { return *regst_desc_; }
  const HashSet<int64_t>& lifetime_actor_ids() const { return *lifetime_actor_ids_; }

 private:
  const RegstDescProto* regst_desc_;
  std::unique_ptr<HashSet<int64_t>> lifetime_actor_ids_;
};

class RegstLifetimeGraph final : public Graph<const RegstLifetimeNode, RegstLifetimeEdge> {
 public:
  OF_DISALLOW_COPY_AND_MOVE(RegstLifetimeGraph);
  RegstLifetimeGraph(
      const std::list<const RegstDescProto*>& regst_descs,
      const std::function<void(const RegstDescProto*, HashSet<int64_t>*)>& ComputeLifetimeActorIds);
  ~RegstLifetimeGraph() = default;

  void ForEachSameColoredRegstDescs(
      const std::function<void(const std::list<const RegstDescProto*>&)>& Handler) const;

 private:
  void InitNodes(
      const std::list<const RegstDescProto*>& regst_descs,
      const std::function<void(const RegstDescProto*, HashSet<int64_t>*)>& ComputeLifetimeActorIds,
      std::list<RegstLifetimeNode*>* nodes);
  void InitEdges(const std::list<RegstLifetimeNode*>& nodes);
  HashMap<const RegstLifetimeNode*, HashSet<const RegstLifetimeNode*>>
      regst_lifetime_node2intersected_nodes_;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_GRAPH_REGST_LIFETIME_GRAPH_H_
