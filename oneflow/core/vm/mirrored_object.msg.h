#ifndef ONEFLOW_CORE_VM_MIRRORED_OBJECT_MSG_H_
#define ONEFLOW_CORE_VM_MIRRORED_OBJECT_MSG_H_

#include "oneflow/core/common/flat_msg.h"
#include "oneflow/core/common/object_msg.h"
#include "oneflow/core/vm/object_id_util.h"
#include "oneflow/core/vm/mirrored_object_id.msg.h"
#include "oneflow/core/vm/stream_desc.msg.h"
#include "oneflow/core/vm/object.h"
#include "oneflow/core/job/parallel_desc.h"

namespace oneflow {

namespace vm {

class Instruction;
class MirroredObject;

// clang-format off
OBJECT_MSG_BEGIN(RwMutexedObjectAccess);
  // methods
  PUBLIC void __Init__(Instruction* instruction, MirroredObject* mirrored_object,
                       bool is_const_operand);

  // fields
  OBJECT_MSG_DEFINE_OPTIONAL(bool, is_const_operand);
  OBJECT_MSG_DEFINE_PTR(Instruction, instruction);
  OBJECT_MSG_DEFINE_PTR(MirroredObject, mirrored_object);

  // links
  OBJECT_MSG_DEFINE_LIST_LINK(rw_mutexed_object_access_link);
  OBJECT_MSG_DEFINE_SKIPLIST_KEY(10, MirroredObjectId, mirrored_object_id);
  
OBJECT_MSG_END(RwMutexedObjectAccess);
// clang-format on

// clang-format off
OBJECT_MSG_BEGIN(HostMemBuffer);
  // methods
  PUBLIC void __Init__(size_t size, char* data) {
    set_size(size);
    set_data(data);
  }

  // fields
  OBJECT_MSG_DEFINE_OPTIONAL(size_t, size);
  OBJECT_MSG_DEFINE_PTR(char, data);
OBJECT_MSG_END(HostMemBuffer);
// clang-format on

// clang-format off
OBJECT_MSG_BEGIN(CudaMemBuffer);
  // methods
  PUBLIC void __Init__(size_t size, char* data) {
    set_size(size);
    set_data(data);
  }

  // fields
  OBJECT_MSG_DEFINE_OPTIONAL(size_t, size);
  OBJECT_MSG_DEFINE_PTR(char, data);
OBJECT_MSG_END(CudaMemBuffer);

class LogicalObject;
OBJECT_MSG_BEGIN(RwMutexedObject);
  // methods

  PUBLIC template<typename T> bool Has() const {
    return dynamic_cast<const T*>(&object()) != nullptr;
  }
  PUBLIC template<typename T> const T& Get() const {
    const T* obj = dynamic_cast<const T*>(&object());
    CHECK(obj != nullptr);
    return *obj;
  }
  PUBLIC template<typename T> T* Mut() {
    T* object = dynamic_cast<T*>(object_ptr().get());
    CHECK(object != nullptr);
    return object;
  }
  PUBLIC template<typename T, typename... Args> T* Init(Args&&... args) {
    T* object = dynamic_cast<T*>(object_ptr().get());
    CHECK(object == nullptr);
    object = new T(std::forward<Args>(args)...);
    reset_object(object);
    return object;
  }
  PUBLIC const Object& object() const { return *object_ptr().get(); }
  PUBLIC bool has_object() const { return object_ptr().get() != nullptr; }
  PUBLIC void reset_object(Object* object) { mut_object_ptr()->reset(object); }
  PUBLIC void reset_object() { reset_object(nullptr); }

  //fields
  OBJECT_MSG_DEFINE_STRUCT(std::unique_ptr<Object>, object_ptr);

OBJECT_MSG_END(RwMutexedObject);

OBJECT_MSG_BEGIN(MirroredObject);
  // methods
  PUBLIC void __Init__(LogicalObject* logical_object, int64_t global_device_id);

  PUBLIC template<typename T> bool Has() const { return rw_mutexed_object().Has<T>(); }
  PUBLIC template<typename T> const T& Get() const { return rw_mutexed_object().Get<T>(); }
  PUBLIC template<typename T> T* Mut() { return mut_rw_mutexed_object()->Mut<T>(); }
  PUBLIC template<typename T, typename... Args> T* Init(Args&&... args) {
    return mut_rw_mutexed_object()->Init<T, Args...>(std::forward<Args>(args)...);
  }
  PUBLIC const Object& object() const { return rw_mutexed_object().object(); }
  PUBLIC bool has_object() const { return rw_mutexed_object().has_object(); }
  PUBLIC void reset_object(Object* object) { mut_rw_mutexed_object()->reset_object(object); }
  PUBLIC void reset_object() { mut_rw_mutexed_object()->reset_object(); }

  //fields
  OBJECT_MSG_DEFINE_FLAT_MSG(MirroredObjectId, mirrored_object_id);
  OBJECT_MSG_DEFINE_OPTIONAL(RwMutexedObject, rw_mutexed_object);

  // links
  OBJECT_MSG_DEFINE_MAP_KEY(int64_t, global_device_id);
  OBJECT_MSG_DEFINE_LIST_HEAD(RwMutexedObjectAccess, rw_mutexed_object_access_link, access_list);
OBJECT_MSG_END(MirroredObject);

class VirtualMachine;
OBJECT_MSG_BEGIN(LogicalObject);
  // methods
  PUBLIC void __Init__(const ObjectId& logical_object_id) {
    __Init__(logical_object_id, std::shared_ptr<ParallelDesc>());
  }
  PUBLIC void __Init__(const ObjectId& logical_object_id,
                       const std::shared_ptr<ParallelDesc>& parallel_desc) {
    set_logical_object_id(logical_object_id);
    *mutable_parallel_desc() = parallel_desc;
  }

  // fields
  OBJECT_MSG_DEFINE_STRUCT(std::shared_ptr<ParallelDesc>, parallel_desc);

  // links
  OBJECT_MSG_DEFINE_MAP_HEAD(MirroredObject, global_device_id, global_device_id2mirrored_object);
  OBJECT_MSG_DEFINE_MAP_HEAD(MirroredObject, global_device_id, global_device_id2type_mirrored_object);
  OBJECT_MSG_DEFINE_MAP_KEY(ObjectId, logical_object_id);
OBJECT_MSG_END(LogicalObject);
// clang-format on

}  // namespace vm

}  // namespace oneflow

#endif  // ONEFLOW_CORE_VM_MIRRORED_OBJECT_MSG_H_
