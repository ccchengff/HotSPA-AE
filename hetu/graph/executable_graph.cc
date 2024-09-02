#include "hetu/graph/executable_graph.h"
#include "hetu/graph/switch_exec_graph.h"
#include "hetu/graph/profiler.h"
#include "hetu/graph/ops/data_transfer.h"
#include "hetu/graph/ops/group.h"
#include "hetu/graph/ops/Split.h"
#include "hetu/graph/ops/sum.h"
#include "hetu/graph/ops/Concatenate.h"
#include "hetu/graph/ops/Communication.h"
#include "hetu/graph/ops/Contiguous.h"
#include "hetu/graph/ops/Arithmetics.h"
#include "hetu/graph/ops/Loss.h"
#include "hetu/graph/autocast/autocast.h"
#include "hetu/impl/communication/comm_group.h"
#include "hetu/impl/communication/mpi_comm_group.h"
#include "hetu/core/symbol.h"
#include <nccl.h>
#include <ctime>
#include <iostream>
#include <fstream>

namespace hetu {
namespace graph {

static bool is_comm_without_reduce_op(const uint64_t comm_type) {
  return comm_type & (PEER_TO_PEER_SEND_OP | PEER_TO_PEER_RECV_OP |
                      ALL_TO_ALL_OP | ALL_GATHER_OP | BROADCAST_OP |
                      P2P_OP | BATCHED_ISEND_IRECV_OP |
                      GATHER_OP | SCATTER_OP) != 0;
}

static bool is_pipeline_stage_send_op(const Operator& op) {
  if (is_peer_to_peer_send_op(op)) {
    return true;
  }
  if (is_batched_isend_irecv_op(op)) {
    const auto& batched_isend_irecv_op_impl = dynamic_cast<BatchedISendIRecvOpImpl&>(op->body());
    // 只发不收
    if (batched_isend_irecv_op_impl.src_devices().empty()) {
      HT_ASSERT(!batched_isend_irecv_op_impl.dst_devices().empty())
        << "only one side could be empty";
      return true;
    }
  }
  return false;
}

static bool is_fused_pipeline_stage_send_op(const Operator& op) {
  if (is_peer_to_peer_send_op(op)) {
    return true;
  }
  auto cur_op = op;
  while (true) {
    if (is_slice_op(cur_op)) {
      cur_op = cur_op->output(0)->consumer(0);
      continue;
    }
    if (is_batched_isend_irecv_op(cur_op)) {
      const auto& batched_isend_irecv_op_impl = dynamic_cast<BatchedISendIRecvOpImpl&>(cur_op->body());
      // 只发不收
      if (batched_isend_irecv_op_impl.src_devices().empty()) {
        HT_ASSERT(!batched_isend_irecv_op_impl.dst_devices().empty())
          << "only one side could be empty";
        return true;
      }
    }
    break;
  }
  return false;
}

static Operator get_next_pipeline_stage_send_op(const Operator& op) {
  if (is_peer_to_peer_send_op(op)) {
    return op;
  }
  auto cur_op = op;
  while (true) {
    if (is_slice_op(cur_op)) {
      cur_op = cur_op->output(0)->consumer(0);
      continue;
    }
    if (is_batched_isend_irecv_op(cur_op)) {
      const auto& batched_isend_irecv_op_impl = dynamic_cast<BatchedISendIRecvOpImpl&>(cur_op->body());
      // 只发不收
      if (batched_isend_irecv_op_impl.src_devices().empty()) {
        HT_ASSERT(!batched_isend_irecv_op_impl.dst_devices().empty())
          << "only one side could be empty";
        return cur_op;
      }
    }
    break;
  }
  HT_RUNTIME_ERROR << "Please ensure the op is already a fused pipeline send op";
}

static bool is_pipeline_stage_recv_op(const Operator& op) {
  if (is_peer_to_peer_recv_op(op)) {
    return true;
  }
  if (is_batched_isend_irecv_op(op)) {
    const auto& batched_isend_irecv_op_impl = dynamic_cast<BatchedISendIRecvOpImpl&>(op->body());
    // 只收不发
    if (batched_isend_irecv_op_impl.dst_devices().empty()) {
      HT_ASSERT(!batched_isend_irecv_op_impl.src_devices().empty())
        << "only one side could be empty";
      return true;
    }
  }
  return false;
}

static bool is_fused_pipeline_stage_recv_op(const Operator& op) {
  if (is_peer_to_peer_recv_op(op)) {
    return true;
  }
  auto cur_op = op;
  while (true) {
    if (is_concat_op(cur_op)) {
      cur_op = cur_op->input(0)->producer();
      continue;
    }
    if (is_batched_isend_irecv_op(cur_op)) {
      const auto& batched_isend_irecv_op_impl = dynamic_cast<BatchedISendIRecvOpImpl&>(cur_op->body());
      // 只收不发
      if (batched_isend_irecv_op_impl.dst_devices().empty()) {
        HT_ASSERT(!batched_isend_irecv_op_impl.src_devices().empty())
          << "only one side could be empty";
        return true;
      }
    }
    break;
  }
  return false;
}

static Operator get_last_pipeline_stage_recv_op(const Operator& op) {
  if (is_peer_to_peer_recv_op(op)) {
    return op;
  }
  auto cur_op = op;
  while (true) {
    if (is_concat_op(cur_op)) {
      cur_op = cur_op->input(0)->producer();
      continue;
    }
    if (is_batched_isend_irecv_op(cur_op)) {
      const auto& batched_isend_irecv_op_impl = dynamic_cast<BatchedISendIRecvOpImpl&>(cur_op->body());
      // 只收不发
      if (batched_isend_irecv_op_impl.dst_devices().empty()) {
        HT_ASSERT(!batched_isend_irecv_op_impl.src_devices().empty())
          << "only one side could be empty";
        return cur_op;
      }
    }
    break;
  }
  HT_RUNTIME_ERROR << "Please ensure the op is already a fused pipeline recv op";
}

Operator& ExecutableGraph::MakeOpInner(std::shared_ptr<OpInterface> body,
                                       TensorList inputs, OpMeta op_meta) {
  _check_all_inputs_in_graph(inputs, op_meta.extra_deps);
  // Use in DoInferMeta
  // Some ops need a inferred device
  // so that the output tensor shape is determined
  if (op_meta.device_group_hierarchy.size() != 0) {
    DeviceGroupUnion device_group_union;
    if (op_meta.device_group_hierarchy.size() == 1) {
      device_group_union = op_meta.device_group_hierarchy.get(0);
    } else {
      device_group_union = op_meta.device_group_hierarchy.get(CUR_STRATEGY_ID);
    }
    auto inferred = hetu::impl::comm::GetLocalDevice();
    if (device_group_union.has(inferred)) {
      CUR_HETERO_ID = device_group_union.get_index(inferred);
    } else {
      CUR_HETERO_ID = SUGGESTED_HETERO_ID;
    }
  }
  OpRef op_ref = MakeAndAddOp(std::move(body), std::move(inputs), std::move(op_meta));
  CUR_HETERO_ID = 0;
  return op_ref;
}

void ExecutableGraph::ResetVariableDataInner(const Tensor& tensor,
                                             const Initializer& init) {
  if (tensor->placement().is_undetermined()) {
    _add_on_inits[tensor->id()] = std::unique_ptr<Initializer>(init.copy());
  } else {
    init.Init(GetVariableDataInner(tensor));
  }
}

NDArray& ExecutableGraph::GetVariableDataInner(const Tensor& tensor) {
  auto it = _preserved_data.find(tensor->id());
  HT_RUNTIME_ERROR_IF(it == _preserved_data.end())
    << "Cannot find data for variable tensor " << tensor;
  return it->second;
}

NDArray ExecutableGraph::GetDetachedVariableDataInner(const Tensor& tensor) {
  // Question: store the data on different devices? For now, store all on CPU and return.
  auto it_1 = _preserved_data.find(tensor->id());
  if (it_1 == _preserved_data.end()) {
    auto it_2 = _add_on_inits.find(tensor->id());
    // haven't alloc yet
    if (it_2 != _add_on_inits.end()) {
      auto ret = NDArray::empty(tensor->shape(), Device(kCPU), tensor->dtype(), kBlockingStream);
      HT_LOG_TRACE << "The data is in executable graph, but not allocated yet, so getting the data of the variable from its initializer.";
      it_2->second->Init(ret);
      return ret;
    }
    else {
      HT_RUNTIME_ERROR << "Cannot find data in executable graph for variable tensor " << tensor;
    }
  }
  HT_LOG_TRACE << "Fetch the data from the executable graph.";
  return NDArray::to(it_1->second, Device(kCPU));
}

NDArray& ExecutableGraph::AllocVariableDataInner(const Tensor& tensor,
                                                 const Initializer& init,
                                                 uint64_t seed,
                                                 const HTShape& global_shape) {
  if (_preserved_data.find(tensor->id()) != _preserved_data.end()) {
    HT_LOG_DEBUG << hetu::impl::comm::GetLocalDevice() << ": exec variable " << tensor 
      << " already has the data, so we directly return it";
    return _preserved_data[tensor->id()];
  }
  HT_LOG_DEBUG << hetu::impl::comm::GetLocalDevice() << ": alloc exec variable " << tensor;
  // TODO: check meta is valid & maybe we can use non-blocking stream?
  bool is_param = (_parameter_ops.find(tensor->producer()->id()) != _parameter_ops.end());
  bool is_optvar = (_optimizer_variable_ops.find(tensor->producer()->id()) != _optimizer_variable_ops.end());
  if ((_use_origin_param_and_optimizer_buffer || _use_origin_param_and_optimizer_buckets) && (is_param || is_optvar)) {
    if (_use_origin_param_and_optimizer_buckets) {
      HT_ASSERT(_origin_param_and_optimizer_buckets->HasTensor(tensor))
        << "Cannot find param " << tensor << " in the origin param and optimizer buckets";
      // alloc on-the-fly
      auto bucket = _origin_param_and_optimizer_buckets->GetTensorBucket(tensor);
      if (!bucket->IsAllocated()) {
        bucket->Alloc(Stream(tensor->placement(), kBlockingStream));
      }
      _preserved_data[tensor->id()] = NDArray(tensor->meta(), 
                                              bucket->AsStorage(), 
                                              bucket->GetElementOffest(tensor));
    }
    // deprecated: 目前使用buckets
    else if (_use_origin_param_and_optimizer_buffer) {
      HT_ASSERT(_origin_param_and_optimizer_buffer->HasTensor(tensor))
        << "Cannot find param " << tensor << " in the origin param and optimizer buffer";
      // alloc on-the-fly
      if (!_origin_param_and_optimizer_buffer->IsAllocated()) {
        _origin_param_and_optimizer_buffer->Alloc(Stream(tensor->placement(), kBlockingStream));
      }
      _preserved_data[tensor->id()] = NDArray(tensor->meta(), 
                                              _origin_param_and_optimizer_buffer->AsStorage(), 
                                              _origin_param_and_optimizer_buffer->GetElementOffest(tensor));
    }
  } 
  // deprecated:
  // 目前一定会使用origin_param_and_optimizer_buffer或者buckets
  else if (!_use_origin_param_and_optimizer_buffer && !_use_origin_param_and_optimizer_buckets && is_param) {
    HT_ASSERT(_origin_param_buffer->HasTensor(tensor))
      << "Cannot find param " << tensor << " in the origin param buffer";
    // alloc on-the-fly
    if (!_origin_param_buffer->IsAllocated()) {
      _origin_param_buffer->Alloc(Stream(tensor->placement(), kBlockingStream));
    }
    _preserved_data[tensor->id()] = NDArray(tensor->meta(), 
                                            _origin_param_buffer->AsStorage(), 
                                            _origin_param_buffer->GetElementOffest(tensor));
  }
  // 其余不在buffer中
  else {
    // 另外一些是variable但不是param/optvar的正常走mempool
    // 分配的是碎片化的显存
    // mempool debug use
    HT_LOG_TRACE << hetu::impl::comm::GetLocalDevice() << ": on-the-fly alloc variable " << tensor
      << ", shape = " << tensor->shape() << ", placement = " << tensor->placement();
    _preserved_data[tensor->id()] = NDArray::empty(tensor->shape(), 
                                                   tensor->placement(), 
                                                   tensor->dtype(), 
                                                   kBlockingStream);
  }
  auto it = _add_on_inits.find(tensor->id());
  if (it != _add_on_inits.end()) {
    it->second->Init(_preserved_data[tensor->id()], seed, global_shape,
                     kBlockingStream);
  } else if (!init.vodify()) {
    init.Init(_preserved_data[tensor->id()], seed, global_shape,
              kBlockingStream);
  }
  return _preserved_data[tensor->id()];
}

void ExecutableGraph::RegisterVariableDataInner(const Tensor& tensor,
                                                NDArray data,
                                                const Initializer& init) {
  _preserved_data[tensor->id()] = std::move(data);
  auto it = _add_on_inits.find(tensor->id());
  if (it != _add_on_inits.end()) {
    it->second->Init(_preserved_data[tensor->id()]);
  } else if (!init.vodify()) {
    init.Init(_preserved_data[tensor->id()]);
  }
}

void ExecutableGraph::AllocRuntimeBuffer(std::vector<RuntimeContext>& runtime_ctx_list) {
  // some memory could alloc in advance
  // 1、fragile non-param varaible (alloc and compute)
  // 2、origin param (if needed, alloc on-the-fly and compute)
  // 3、transfer param (alloc and compute)
  // 4、grad (just alloc)
  auto local_device = hetu::impl::comm::GetLocalDevice();
  // ---------- param ----------
  if (!_transfer_param_buffer->IsEmpty() && !_transfer_param_buffer->IsAllocated()) {
    // alloc transfer param
    _transfer_param_buffer->Alloc(Stream(local_device, kBlockingStream));
    HT_LOG_DEBUG << local_device << ": alloc transfer param buffer "
      << ", the size is " << _transfer_param_buffer->size();
  }
  for (auto& op_ref : _execute_plan.local_placeholder_variable_ops) {
    auto& op = op_ref.get();
    if (is_variable_op(op)) {
      // 是param且存在data transfer的情况需要单独处理
      // 因为有可能是热切换过来的而不需要再计算
      if (_parameter_ops.find(op->id()) != _parameter_ops.end() && !_transfer_param_buffer->IsEmpty()) {
        auto it = _transfer_map.find(op->output(0)->id());
        HT_ASSERT(it != _transfer_map.end())
          << "The transfer map does not consist of " << op->output(0);
        auto& transfer_param = it->second;
        auto transfer_param_data = NDArray(transfer_param->meta(),
                                          _transfer_param_buffer->AsStorage(), 
                                          _transfer_param_buffer->GetElementOffest(transfer_param));
        // 添加runtime allocation
        for (auto& runtime_ctx : runtime_ctx_list) {
          runtime_ctx.add_runtime_allocation(transfer_param->id(), transfer_param_data);
        }
        // 热切换
        if (_preserved_data.find(transfer_param->id()) != _preserved_data.end()) {
          HT_LOG_DEBUG << hetu::impl::comm::GetLocalDevice() << ": exec transfer param " 
            << transfer_param << " already has the data";
        } 
        // 冷启动
        // 这种也包括了单策略一直训练
        // 即每次不需要再单独分配transfer param buffer但需要重新进行transfer
        else {
          // alloc and compute origin param
          auto origin_param = op->Compute({}, runtime_ctx_list[0]);
          // compute transfer param
          transfer_param->producer()->Compute(origin_param, runtime_ctx_list[0]);
          // todo: for pp > 1, it is safer to 
          // record the start and end event on all micro batches here
          _preserved_data[transfer_param->id()] = transfer_param_data;
        }
        // 添加runtime skipped
        for (auto& runtime_ctx : runtime_ctx_list) {
          runtime_ctx.add_runtime_skipped(op->id());
          runtime_ctx.add_runtime_skipped(transfer_param->producer()->id());
        }
      }
      // 其余情况正常按variable去compute即可
      // AllocVariableDataInner已经自动处理了_preserved_data已存在的情况
      else {
        // alloc阶段只分配param
        if (_run_level == RunLevel::ALLOC) {
          continue;
        }
        // grad阶段optimizer相关的variable不用跑
        // 例如Adam的step、mean、variance
        if (_run_level == RunLevel::GRAD) {
          if (op->output(0)->num_consumers() == 1 
              && is_optimizer_update_op(op->output(0)->consumer(0))) {
            continue;
          }
        }
        op->Compute({}, runtime_ctx_list[0]);
        // 添加runtime skipped
        for (auto& runtime_ctx : runtime_ctx_list) {
          runtime_ctx.add_runtime_skipped(op->id());
        }
      }
    }
  } 
  // ---------- grad ----------
  if (_run_level == RunLevel::GRAD || _run_level == RunLevel::UPDATE) {
    if (_use_current_grad_buffer) {
      if (!_current_grad_buffer->IsEmpty() && !_current_grad_buffer->IsAllocated()) {
        // alloc grad
        _current_grad_buffer->Alloc(Stream(local_device, kBlockingStream));
        HT_LOG_DEBUG << local_device << ": alloc current grad buffer "
          << ", the size is " << _current_grad_buffer->size();
      }
      for (const auto& current_grad : _current_grad_buffer->tensor_list()) {
        auto current_grad_data = NDArray(current_grad->meta(),
                                         _current_grad_buffer->AsStorage(), 
                                         _current_grad_buffer->GetElementOffest(current_grad));
        // 添加runtime allocation
        for (auto& runtime_ctx : runtime_ctx_list) {
          auto it = _grad_grad_map.find(current_grad->id());
          HT_ASSERT(it != _grad_grad_map.end())
            << "cannot find the mapping of " << current_grad << " in the grad grad map";
          runtime_ctx.add_runtime_allocation(it->second->id(), current_grad_data);
        }
        // 注意与param不同的是
        // 这里不能添加runtime skipped
        // 因为grad还是要计算的
      }
    }
    // 使用accumulate_grad_buffer
    // 初始全为0
    else {
      if (_run_level == RunLevel::GRAD 
          && !_accumulate_grad_buffer->IsEmpty() 
          && !_accumulate_grad_buffer->IsAllocated()) {
        _accumulate_grad_buffer->Alloc(Stream(local_device, kBlockingStream));
        auto accumulate_grad_buffer_data = _accumulate_grad_buffer->AsNDArray();
        NDArray::zeros_(accumulate_grad_buffer_data, kBlockingStream);
      }
    }
  }
}

bool ExecutableGraph::Instantiate(const TensorList& fetches,
                                  const Device& preferred_device) {
  auto is_op_instantiated = [&](const Operator& op) -> bool {
    return !op->placement().is_undetermined();
  };
  OpRefList topo = Graph::TopoSort(fetches, num_ops(), is_op_instantiated);
  HT_LOG_TRACE << "Instantiating ops: " << topo;

  HT_LOG_DEBUG << "global info for all devices begin...";
  // global info for all devices
  for (auto& op_ref : topo) {
    auto& op = op_ref.get();
    if (!op->placement().is_undetermined())
      continue;
    // 处理1
    // handle unused or redundant comm ops
    if (is_comm_op(op) && op->placement_group_union().has(preferred_device)) {
      auto& comm_op_impl = dynamic_cast<CommOpImpl&>(op->body());
      // 1. remove unused comm ops
      if (comm_op_impl.get_comm_type(op, preferred_device) == UNUSED_OP) {
        // HT_LOG_WARN << op << " is an unused comm op and will be removed";
        // should remove consumer of unused comm_op from end to begin
        for (int i = op->output(0)->num_consumers() - 1; i >= 0; i--) {
          auto& consumer_i = op->output(0)->consumer(i);
          for (int j = 0; j < consumer_i->num_inputs(); j++) {
            if (consumer_i->input(j)->id() == op->output(0)->id()) {
              Graph::ReplaceInput(consumer_i, j, op->input(0));
            }
          }
        }
        continue;
      }
      // 2. fuse redundant comm ops
      auto& input_op = op->input(0)->producer();
      if (is_comm_op(input_op)) {
        // 尝试融合input op和op两个comm算子
        // *目前只支持将不含partial的两个算子融合成一个BatchedIsendIrecv
        auto& input_comm_op_impl = dynamic_cast<CommOpImpl&>(input_op->body());
        if (is_comm_without_reduce_op(input_comm_op_impl.get_comm_type(input_op, preferred_device))
             && is_comm_without_reduce_op(comm_op_impl.get_comm_type(op, preferred_device))) {
          HT_LOG_WARN << "Fuse " << input_op << " with type " << input_comm_op_impl.get_comm_type(input_op, preferred_device)
            << " and " << op << " with type " << comm_op_impl.get_comm_type(op, preferred_device);
          Graph::ReplaceInput(op, 0, input_op->input(0));
          // input changes, update comm_op type
          comm_op_impl.get_comm_type(op, preferred_device);
        }
      }
    }
    // 处理2
    // loss & grad should div by num_micro_batches when reduction type = MEAN
    if (is_loss_gradient_op(op) && op->input(0)->has_distributed_states()) {
      int dp = op->input(0)->get_distributed_states().get_dim(0);
      auto& loss_grad_op_impl = dynamic_cast<LossGradientOpImpl&>(op->body());
      if ((_num_micro_batches > 1 || dp > 1) && loss_grad_op_impl.reduction() == kMEAN) {
        auto& grads = op->outputs();
        for (auto& grad : grads) {
          if (!grad.is_defined()) {
            continue;
          }
          Tensor grad_scale = MakeDivByConstOp(grad, _num_micro_batches * dp, OpMeta().set_name(grad->name() + "_scale"));
          RecordExecTensor(grad_scale);
          auto& grad_scale_op = grad_scale->producer();
          grad_scale_op->MapToParallelDevices(op->placement_group_union());
          for (int i = grad->num_consumers() - 1; i >= 0; i--) {
            auto& consumer_i = grad->consumer(i);
            if (consumer_i->id() == grad_scale_op->id()) continue;
            for (int j = 0; j < consumer_i->num_inputs(); j++) {
              if (consumer_i->input(j)->id() == grad->id()) {
                Graph::ReplaceInput(consumer_i, j, grad_scale);
              }
            }
          }
        }
      }
    }
    // TODO: 处理3
    // if consecutive ops have different placement groups
    // need to insert comm op automatically
    // 目前已在py端手动插入 
  }
  // HT_LOG_WARN << "global info for all devices end...";
  
  // get updated topo
  OpRefList updated_topo = Graph::TopoSort(fetches, num_ops(), is_op_instantiated);
  HT_LOG_DEBUG << "local info for local_device begin...";
  // local info for local_device
  for (auto& op_ref : updated_topo) {
    auto& op = op_ref.get();
    // HT_LOG_WARN << op << " placement group union is " << op->placement_group_union();
    if (!op->placement().is_undetermined())
      continue;  
    
    Device preferred_device_ = preferred_device;
    if (op->op_meta().is_step)
      preferred_device_ = kCPU;
    else if (!op->placement_group_union().has(preferred_device_)) // for local compute: op->placement + tensor->placement
      continue;
    Device placement =
      is_device_to_host_op(op) ? Device(kCPU) : preferred_device_;
    StreamIndex stream_id = get_suggested_stream_index(op);
    HT_LOG_TRACE << "Instantiating op " << op << " (placement=" << placement
                 << ", stream_index=" << stream_id << ")";
    bool ok = op->Instantiate(placement, stream_id);
    if (!ok && !placement.is_cpu()) {
      HT_LOG_WARN << "Failed to instantiate op " << op << " on " << placement
                  << ". Will try to instantiate it on the host device.";
      placement = Device(kCPU);
      ok = op->Instantiate(placement, stream_id);
    }
    HT_VALUE_ERROR_IF(!ok) << "Failed to instantiate op " << op << " on "
                           << placement;

    // add transfer ops
    for (size_t i = 0; i < op->num_inputs(); i++) {
      auto& input = op->input(i);
      if (op->type() == "AdamOp" && i == 4)
        continue;
      if (input->placement() != placement && !is_comm_op(op)) {
        HT_LOG_WARN << op << " placement is " << placement
          << ", but input " << input << " placement is " << input->placement()
          << ", so needs to add data transfer op";
        HT_RUNTIME_ERROR_IF(input->placement().is_undetermined())
          << input << " placement is undetermined";
        HT_RUNTIME_ERROR_IF(!input->placement().local())
          << "Please use P2P communication to fetch remote input";
        auto& input_op = input->producer();
        Tensor transferred_input;
        StreamIndex transfer_stream_id;
        if (input->placement().is_cpu()) {
          transferred_input = MakeDataH2DOp(placement, input);
          transfer_stream_id = kH2DStream;
        } else if (placement.is_cpu()) {
          transferred_input = MakeDataD2HOp(placement, input);
          transfer_stream_id = kD2HStream;
        } else {
          // TODO: support cuda memcpy across processes
          HT_NOT_IMPLEMENTED << "We should use NCCL for P2P communication: " << op->type();
          __builtin_unreachable();
        }
        RecordExecTensor(transferred_input);
        auto& transfer_op = transferred_input->producer();
        if (!input_op->placement_group_union().size() == 0)
          transfer_op->MapToParallelDevices(input_op->placement_group_union());
        transfer_op->Instantiate(placement, transfer_stream_id);
        Graph::ReplaceInput(op, i, transferred_input);
      }
    }
  }
  HT_LOG_DEBUG << "local info for local_device end...";
  return true;
}

void ExecutableGraph::InsertContiguousOp(const OpRefList& topo_order) {
  auto& local_device = hetu::impl::comm::GetLocalDevice();
  for (auto& op_ref : topo_order) {
    auto& op = op_ref.get();
    if (op->body().require_contig_inputs()) {
      for (size_t i = 0; i < op->num_inputs(); i++) {
        auto& input = op->input(i);
        auto& input_op = input->producer();
        if (!input_op->placement_group_union().has(local_device))
          continue;
        if (!input->is_contiguous()) {
          auto op_id = input->get_contiguous_op_id();
          if (op_id.has_value() &&
              _op_indexing[op_id.value()]->placement() == local_device) {
            HT_LOG_TRACE << "Tensor " << input->name()
                         << " is not contiguous for op " << op->body().type()
                         << ". But it may have a contiguous copy, use it instead";
            auto contig_op = _op_indexing[op_id.value()];
            Graph::ReplaceInput(op, i, contig_op->output(0));
          } else {
            HT_LOG_TRACE << hetu::impl::comm::GetLocalDevice() << ": Make Contiguous op for tensor " << input->name()
                         << " while making " << op->body().type() << " op.";
            Tensor contig_input = MakeContiguousOp(
              input, OpMeta().set_name(input->name() + "_contig")
                             .set_is_deduce_states(false));
            HT_LOG_TRACE << "Insert contiguous op for " << input
              << ", shape is " << input->shape()
              << ", stride is " << input->stride();
            RecordExecTensor(contig_input);
            auto& contig_op = contig_input->producer();
            contig_op->MapToParallelDevices(input_op->placement_group_union());
            contig_op->Instantiate(local_device, kComputingStream);
            Graph::ReplaceInput(op, i, contig_input);
          }
        }
      }
    }
  }
}

void ExecutableGraph::SubstituteCommOp(const OpRefList& topo_order) {
  auto& local_device = hetu::impl::comm::GetLocalDevice();
  for (auto& op_ref : topo_order) {
    auto& op = op_ref.get();
    // each device only need to substitute local comm_ops
    if (is_comm_op(op) && op->placement_group_union().has(local_device)) {
      HT_LOG_DEBUG << local_device << "==============> substitute comm_op begin: " << op << "...";
      auto& comm_op = op;
      auto& comm_op_impl = dynamic_cast<CommOpImpl&>(comm_op->body());
      const auto& info = comm_op_impl.get_comm_info(comm_op, local_device);
      // HT_LOG_WARN << comm_op << ": " << info;
      uint64_t comm_type = comm_op_impl.get_comm_type(comm_op, local_device, info);
      Tensor& input = comm_op->input(0);
      // 标记通信算子的输入具有symbolic shape
      if (!input->symbolic()) {
        input->init_symbolic_shape();
      }
      bool ignore_flag = false, local_comm_flag = false, determine_flag = false;
      Tensor result = input;

      if ((comm_type & UNUSED_OP) != 0) {
        HT_RUNTIME_ERROR << "Unused comm op should already be deleted when instantiating";
      }
      if ((comm_type & BATCHED_ISEND_IRECV_OP) != 0) {
        // 1. local_device send data to other devices 
        // 2. local_device recv data from other devices
        // use derived method from switch exec graph
        auto complex_exec_comm = ComplexExecComm(comm_op, info);
        result = complex_exec_comm.Instantiate();
        HT_LOG_DEBUG << local_device << ": substitute comm_op to batched_isend_irecv_op";
        determine_flag = true;
      }
      if ((comm_type & P2P_OP) != 0) {
        Tensor& output = comm_op->output(0); // output meta was already deduced in DoInferMeta
        HT_ASSERT(output->shape() == result->shape())
          << "p2p shape should be equal";
        // p2p send
        if (info.src_group.contains(local_device)) {
          // 自己发给自己
          if (info.dst_group.get(info.src_group.get_index(local_device)) == local_device) {
            HT_LOG_DEBUG << local_device << ": redundant p2p send from " 
              << info.src_group << " to " << info.dst_group;
          } 
          // 发给别人
          else {
            HT_LOG_DEBUG << local_device << ": send from stage " << info.src_group << " to " << info.dst_group;
            Tensor send_out_dep_linker = MakeP2PSendOp(
              result, info.dst_group, info.src_group.get_index(local_device), 
              OpMeta().set_is_deduce_states(false));
            // since send_out_dep_linker has an empty shape and is useless, recording its shape is unnecessary
            // but here we still do it to make the code looks more consistent
            RecordExecTensor(send_out_dep_linker);
            auto& send_op = send_out_dep_linker->producer();
            send_op->MapToParallelDevices(info.src_group_union);
            send_op->Instantiate(local_device, kP2PStream);
            // add dummy link for topo sort
            for (int i = 0; i < comm_op->output(0)->num_consumers(); i++) {
              Graph::AddInDeps(comm_op->output(0)->consumer(i), {send_out_dep_linker});
            }
          }
        }
        // p2p recv
        else {
          HT_ASSERT(info.dst_group.contains(local_device))
            << "dst group must contain local device";
          // 自己收自己
          if (info.src_group.get(info.dst_group.get_index(local_device)) == local_device) {
            HT_LOG_DEBUG << local_device << ": redundant p2p recv from " 
              << info.src_group << " to " << info.dst_group;
          } 
          // 自己收别人
          else {
            HT_LOG_DEBUG << local_device << ": just recv from stage " << info.src_group << " to " << info.dst_group;
            Tensor recv_output = MakeP2PRecvOp(
              info.src_group, output->dtype(), result->symbolic_shape(),
              info.dst_group.get_index(local_device), 
              OpMeta().set_is_deduce_states(false));
            RecordExecTensor(recv_output);
            auto& recv_op = recv_output->producer();
            recv_op->MapToParallelDevices(info.dst_group_union);
            recv_op->Instantiate(local_device, kP2PStream);
            // add dummy link for topo sort
            Graph::AddInDeps(recv_op, {result});
            result = recv_output;
          }
        }
        determine_flag = true;
      }
      if ((comm_type & COMM_SPLIT_OP) != 0) {
        HT_ASSERT(info.src_group == info.dst_group)
          << "wrong src and dst group relationship for " << comm_op
          << ", src_group = " << info.src_group << " and dst group = " << info.dst_group;
        auto local_device_index = info.src_group.get_index(local_device);
        const auto& local_dst_ds = info.local_dst_ds;
        auto cur_state_index = local_dst_ds.map_device_to_state_index(local_device_index);
        const auto& order = local_dst_ds.get_order();
        const auto& states = local_dst_ds.get_states();
        HTAxes keys; 
        HTShape indices, splits;
        for (auto o : order) {
          if (o >= 0) { 
            keys.push_back(o);
            splits.push_back(states.at(o));
            indices.push_back(cur_state_index[o]);
          }
        }
        HT_LOG_DEBUG << local_device << ": keys = " << keys << "; indices = " << indices << "; splits = " << splits;
        Tensor split_output = MakeSplitOp(
          result, keys, indices, splits, 
          OpMeta().set_is_deduce_states(false));
        RecordExecTensor(split_output);
        auto& split_op = split_output->producer();
        split_op->MapToParallelDevices(info.src_group_union);
        split_op->Instantiate(local_device, kComputingStream);
        result = split_output;
        HT_LOG_DEBUG << local_device << ": substitute comm_op to split_op";        
        determine_flag = true;
        local_comm_flag = true;
      }
      if ((comm_type & SCATTER_OP) != 0) {
        HT_ASSERT(info.src_group == info.dst_group)
          << "wrong src and dst group relationship!";
        auto local_device_index = info.src_group.get_index(local_device);
        auto cur_state_index = info.local_dst_ds.map_device_to_state_index(local_device_index);
        auto split_num = info.local_dst_ds.get_dim(0) / info.local_src_ds.get_dim(0);
        auto indice = cur_state_index[0] % split_num;
        HTAxes keys = {0};
        HTShape indices = {indice};
        HTShape splits = {split_num};
        Tensor scatter_output = MakeSplitOp(result, keys, indices, splits, OpMeta().set_is_deduce_states(false));
        RecordExecTensor(scatter_output);
        auto& scatter_op = scatter_output->producer();
        scatter_op->MapToParallelDevices(info.src_group_union);
        scatter_op->Instantiate(local_device, kComputingStream);
        result = scatter_output;
        HT_LOG_DEBUG << local_device << ": substitute comm_op to scatter_op";      
        determine_flag = true;
        local_comm_flag = true;
      }
      if ((comm_type & ALL_REDUCE_OP) != 0) {
        HT_ASSERT(info.src_group == info.dst_group)
          << "wrong src and dst group relationship!";
        DeviceGroup comm_group = comm_op_impl.get_devices_by_dim(comm_op, -2); // do allreduce among comm_group
        Tensor all_reduce_output = MakeAllReduceOp(
          result, comm_group, // comm_group is a subset of placement_group
          comm_op_impl.reduction_type(), false,
          OpMeta().set_is_deduce_states(false)
                  .set_name(result->name() + "_AllReduce"));
        RecordExecTensor(all_reduce_output);
        auto& all_reduce_op = all_reduce_output->producer();
        all_reduce_op->MapToParallelDevices(info.src_group_union);
        all_reduce_op->Instantiate(local_device, kCollectiveStream);
        result = all_reduce_output;
        HT_LOG_DEBUG << local_device << ": substitute comm_op to all_reduce_op: " << comm_group; 
        /*   
        HT_LOG_WARN << local_device << ": " << all_reduce_output << " src ds " << info.src_ds_union.ds_union_info()
          << ", and dst ds is " << info.dst_ds_union.ds_union_info(); 
        */
        determine_flag = true;
        local_comm_flag = true;
      } 
      if ((comm_type & ALL_GATHER_OP) != 0) {
        HT_ASSERT(info.src_group == info.dst_group)
          << "wrong src and dst group relationship!";
        // DeviceGroup comm_group = comm_op_impl.get_devices_by_dim(comm_op, 0);
        int32_t local_device_idx = info.dst_group.get_index(local_device);
        DeviceGroup comm_group = info.local_dst_ds.get_devices_by_dim(-1, local_device_idx, info.dst_group);
        Tensor all_gather_output = MakeAllGatherOp(
          result, comm_group,
          OpMeta().set_is_deduce_states(false)
                  .set_name(result->name() + "_AllGather"));
        RecordExecTensor(all_gather_output);
        auto& all_gather_op = all_gather_output->producer();
        all_gather_op->MapToParallelDevices(info.src_group_union);
        all_gather_op->Instantiate(local_device, kCollectiveStream);
        result = all_gather_output;
        HT_LOG_DEBUG << local_device << ": substitute comm_op to all_gather_op: " << comm_group;
        determine_flag = true;
        local_comm_flag = true;
      }
      if ((comm_type & REDUCE_SCATTER_OP) != 0) {
        HT_ASSERT(info.src_group == info.dst_group)
          << "wrong src and dst group relationship!";
        DeviceGroup comm_group = comm_op_impl.get_devices_by_dim(comm_op, -2);
        Tensor reduce_scatter_output =  MakeReduceScatterOp(
          result, comm_group,
          comm_op_impl.reduction_type(), false,
          OpMeta().set_is_deduce_states(false)
                  .set_name(result->name() + "_ReduceScatter"));
        RecordExecTensor(reduce_scatter_output);
        auto& reduce_scatter_op = reduce_scatter_output->producer();
        reduce_scatter_op->MapToParallelDevices(info.src_group_union);
        reduce_scatter_op->Instantiate(local_device, kCollectiveStream);
        result = reduce_scatter_output;
        HT_LOG_DEBUG << local_device << ": substitute comm_op to reduce_scatter_op: " << comm_group;
        determine_flag = true;
        local_comm_flag = true;
      }
      if ((comm_type & SPLIT_ALL_REDUCE_OP) != 0) {
        HT_ASSERT(info.src_group_union.check_equal(info.dst_group_union))
          << "wrong src and dst group relationship!";
        // 先前进行了局部通信
        // 对齐了所有的local ds
        DistributedStatesUnion intermediate_ds_union(info.dst_ds_union);
        if (local_comm_flag) {
          intermediate_ds_union.change_hetero_dim(info.src_ds_union.hetero_dim());
          result->set_cur_ds_union(intermediate_ds_union); 
        }
        size_t split_num = 0;
        std::vector<DeviceGroupList> comm_groups_list;
        std::tie(split_num, comm_groups_list) = comm_op_impl.get_split_comm_groups_list(comm_op, info.src_group_union, intermediate_ds_union);
        Tensor split_all_reduce_output = MakeSplitAllReduceOp(
          result, comm_groups_list, split_num, 
          comm_op_impl.reduction_type(), false,
          OpMeta().set_is_deduce_states(false)
                  .set_name(result->name() + "_SplitAllReduce"));
        RecordExecTensor(split_all_reduce_output);
        auto& split_all_reduce_op = split_all_reduce_output->producer();
        split_all_reduce_op->MapToParallelDevices(info.src_group_union);
        split_all_reduce_op->Instantiate(local_device, kCollectiveStream);
        result = split_all_reduce_output;
        HT_LOG_DEBUG << local_device << ": substitute comm_op to split_all_reduce_op: " << comm_groups_list;         
        determine_flag = true;
      }
      if ((comm_type & SPLIT_REDUCE_SCATTER_OP) != 0) {
        HT_ASSERT(info.src_group_union.check_equal(info.dst_group_union))
          << "wrong src and dst group relationship!";
        // 先前进行了局部通信
        // 对齐了所有的local ds
        DistributedStatesUnion intermediate_ds_union(info.dst_ds_union);
        if (local_comm_flag) {
          intermediate_ds_union.change_hetero_dim(info.src_ds_union.hetero_dim());
          result->set_cur_ds_union(intermediate_ds_union); 
        }
        size_t split_num = 0;
        std::vector<DeviceGroupList> comm_groups_list;
        std::tie(split_num, comm_groups_list) = comm_op_impl.get_split_comm_groups_list(comm_op, info.src_group_union, intermediate_ds_union);
        Tensor split_reduce_scatter_output = MakeSplitReduceScatterOp(
          result, comm_groups_list, split_num, 
          comm_op_impl.reduction_type(), false,
          OpMeta().set_is_deduce_states(false)
                  .set_name(result->name() + "_SplitReduceScatter"));
        RecordExecTensor(split_reduce_scatter_output);
        auto& split_reduce_scatter_op = split_reduce_scatter_output->producer();
        split_reduce_scatter_op->MapToParallelDevices(info.src_group_union);
        split_reduce_scatter_op->Instantiate(local_device, kCollectiveStream);
        result = split_reduce_scatter_output;
        HT_LOG_DEBUG << local_device << ": substitute comm_op to split_reduce_scatter_op: " << comm_groups_list;         
        determine_flag = true;
      }
      if (!determine_flag) {
        HT_RUNTIME_ERROR << local_device << ": " << comm_op << " type is not supported yet"
          << ", src ds union is " << info.src_ds_union.ds_union_info()
          << ", and dst ds union is " << info.dst_ds_union.ds_union_info()
          << ", src group is " << info.src_group_union
          << ", dst group is " << info.dst_group_union;
      }

      // only send, then need to ignore the shape & ds when replacing the input 
      if (result.get() == input.get()) {
        ignore_flag = true;
      }
      // assign distributed states union for result tensor
      if (!ignore_flag) {
        result->set_cur_ds_union(info.dst_ds_union); 
      }
      // find all comm_op->output consumers, and replace the correspond input tensor with result tensor
      for (int i = comm_op->output(0)->num_consumers() - 1; i >= 0; i--) {
        auto& consumer_i = comm_op->output(0)->consumer(i);
        for (int j = 0; j < consumer_i->num_inputs(); j++) {
          if (consumer_i->input(j)->id() == comm_op->output(0)->id()) {
            Graph::ReplaceInput(consumer_i, j, result, ignore_flag);
          }
        }
      }
      HT_LOG_DEBUG << local_device << "==============> substitute comm_op end: " << op << "...";
    }
  }
}

DeviceGroup ExecutableGraph::GetPrevStage() {
  auto local_device = hetu::impl::comm::GetLocalDevice();
  HT_ASSERT(_pipeline_map.find(local_device) != _pipeline_map.end())
    << "something wrong, can't figure out which pipeline the local device belongs to";
  auto& pipeline = _pipeline_map[local_device];
  int32_t stage_id = -1;
  for (int i = 0; i < pipeline.size(); i++) {
    if (pipeline[i].contains(local_device)) {
      stage_id = i;
    }
  }
  HT_ASSERT(stage_id != -1)
    << "something wrong, can't figure out which stage the local device belongs to";
  HT_ASSERT(stage_id != 0)
    << "the first stage doesn't have any former stage";
  return pipeline.at(stage_id - 1);
}

DeviceGroup ExecutableGraph::GetNextStage() {
  auto local_device = hetu::impl::comm::GetLocalDevice();
  HT_ASSERT(_pipeline_map.find(local_device) != _pipeline_map.end())
    << "something wrong, can't figure out which pipeline the local device belongs to";
  auto& pipeline = _pipeline_map[local_device];
  int32_t stage_id = -1;
  for (int i = 0; i < pipeline.size(); i++) {
    if (pipeline[i].contains(local_device)) {
      stage_id = i;
    }
  }
  HT_ASSERT(stage_id != -1)
    << "something wrong, can't figure out which stage the local device belongs to";
  HT_ASSERT(stage_id != pipeline.size() - 1)
    << "the last stage doesn't have any next stage";
  return pipeline.at(stage_id + 1);
}

// schedule: {stage_id: [<is_forward, micro_batch_id>, <is_forward,
// micro_batch_id>, ...], ...}
std::unordered_map<size_t, std::vector<std::pair<bool, size_t>>>
ExecutableGraph::GenerateGpipeSchedule(
  size_t num_stages, size_t num_micro_batches, bool is_inference) {
  std::unordered_map<size_t, std::vector<std::pair<bool, size_t>>> schedule;
  // inference time: for only forward
  if (is_inference) {
    for (size_t stage_id = 0; stage_id < num_stages; stage_id++) {
      std::vector<std::pair<bool, size_t>> tasks;
      tasks.reserve(num_micro_batches);
      for (size_t step_id = 0; step_id < num_micro_batches; step_id++) {
        tasks.push_back({true, step_id});
      }
      schedule[stage_id] = tasks;
    }
    return schedule;
  }
  // traininig time: for forward and backward
  for (size_t stage_id = 0; stage_id < num_stages; stage_id++) {
    std::vector<std::pair<bool, size_t>> tasks;
    tasks.reserve(2 * num_micro_batches);
    for (size_t step_id = 0; step_id < num_micro_batches; step_id++) {
      tasks.push_back({true, step_id});
    }
    for (size_t step_id = 0; step_id < num_micro_batches; step_id++) {
      tasks.push_back({false, step_id});
    }
    schedule[stage_id] = tasks;
  }
  return schedule;
}

// schedule: {stage_id: [<is_forward, micro_batch_id>, <is_forward,
// micro_batch_id>, ...], ...}
std::unordered_map<size_t, std::vector<std::pair<bool, size_t>>>
ExecutableGraph::GeneratePipedreamFlushSchedule(
  size_t num_stages, size_t num_micro_batches, bool is_inference) {
  // HT_ASSERT(num_micro_batches >= num_stages)
  //   << "num_micro_batches must bigger than num_stages in pipedream-flush"
  //   << ", but find num_micro_batches = " << num_micro_batches
  //   << " and num_stages = " << num_stages;
  std::unordered_map<size_t, std::vector<std::pair<bool, size_t>>> schedule;
  // inference time: for only forward
  if (is_inference) {
    for (size_t stage_id = 0; stage_id < num_stages; stage_id++) {
      std::vector<std::pair<bool, size_t>> tasks;
      tasks.reserve(num_micro_batches);
      for (size_t step_id = 0; step_id < num_micro_batches; step_id++) {
        tasks.push_back({true, step_id});
      }
      schedule[stage_id] = tasks;
    }
    return schedule;
  }
  // traininig time: for forward and backward
  for (size_t stage_id = 0; stage_id < num_stages; stage_id++) {
    std::vector<std::pair<bool, size_t>> tasks;
    tasks.reserve(2 * num_micro_batches);
    size_t num_warmup_microbatches = num_stages - stage_id - 1;
    size_t num_microbatches_remaining =
      num_micro_batches - num_warmup_microbatches;
    // 1. warmup
    for (size_t step_id = 0; step_id < num_warmup_microbatches; step_id++) {
      tasks.push_back({true, step_id});
    }
    // 2. 1F1B
    for (size_t step_id = 0; step_id < num_microbatches_remaining; step_id++) {
      tasks.push_back({true, num_warmup_microbatches + step_id});
      tasks.push_back({false, step_id});
    }
    // 3. cooldown
    for (size_t step_id = 0; step_id < num_warmup_microbatches; step_id++) {
      tasks.push_back({false, num_microbatches_remaining + step_id});
    }
    schedule[stage_id] = tasks;
  }
  return schedule;
}

void ExecutableGraph::ComputeFunc(size_t& micro_batch_id, const OpRefList& topo, RuntimeContext& runtime_ctx,
                                  Tensor2NDArrayMap& tensor2data, Tensor2IntMap& tensor2degrees, 
                                  Tensor2NDArrayMap& grad_accumulation, bool grad_accumulation_finished,
                                  const FeedDict& feed_dict, const TensorList& fetches,
                                  const std::unordered_map<TensorId, size_t>& fetch_indices, bool& is_continuous_p2p) {
  const TensorIdSet& dtype_transfer_tensor = _execute_plan.dtype_transfer_tensor;
  const TensorIdSet& shared_weight_tensor = _execute_plan.shared_weight_tensor;
  const OpIdSet& shared_weight_p2p = _execute_plan.shared_weight_p2p;
  const OpIdSet& shared_weight_grad_p2p = _execute_plan.shared_weight_grad_p2p;
  const TensorIdSet& accumulated_tensor = _execute_plan.accumulated_tensor;
  const OpIdSet& accumulated_ops = _execute_plan.accumulated_ops;

  auto is_shared_weight_or_grad_p2p = [&](const Operator& op) -> bool {
    bool is_shared_weight = (shared_weight_p2p.find(op->id()) != shared_weight_p2p.end());
    bool is_shared_weight_grad = (shared_weight_grad_p2p.find(op->id()) != shared_weight_grad_p2p.end());
    return is_shared_weight || is_shared_weight_grad;
  };

  for (auto& op_ref : topo) {
    auto& op = op_ref.get();
    HT_ASSERT(!is_placeholder_op(op) && !is_variable_op(op))
      << "Placeholder & Variable ops should not appear in ComputeFunc!";
    bool is_feed_dict_op = Operator::all_output_tensors_of(op, [&](Tensor& tensor) {
      return feed_dict.find(tensor->id()) != feed_dict.end();
    });

    if (runtime_ctx.has_runtime_skipped(op->id())) {
      continue; 
    }
    if (is_feed_dict_op) {
      continue;
    }
    // just convert fp32 -> bf16, fp16 in micro batch 0
    // though most of it is actually put in runtime_skipped already
    // but some of it (rotary sin or cos, mask...) is not in runtime_skipped
    if (op->num_outputs() > 0 && dtype_transfer_tensor.find(op->output(0)->id()) != dtype_transfer_tensor.end() && micro_batch_id > 0) {
      // HT_RUNTIME_ERROR << "unreachable";
      continue;
    }
    // in pipeline(shared_weight_p2p not empty), shared weight p2p ops only execute in micro batch 0
    if (!shared_weight_p2p.empty() && shared_weight_p2p.find(op->id()) != shared_weight_p2p.end() && micro_batch_id > 0) {
      // HT_LOG_INFO << hetu::impl::comm::GetLocalDevice() << ": skip execute shared weight p2p: " << op;
      continue;
    }
    // shared weight grad p2p ops are included in accumulated_ops, only execute in last micro batch
    if (!grad_accumulation_finished && accumulated_ops.find(op->id()) != accumulated_ops.end()) {
      continue;
    }
    // GRAD模式和UPDATE模式
    // 只需要再单独考虑optimizer op及之后的算子
    // 其余部分照常
    if (is_group_op(op) && _run_level == RunLevel::GRAD) {
      continue;
    }
    if (is_optimizer_update_op(op)) {
      // 只用得到grad而不需要进行update
      if (_run_level == RunLevel::GRAD) {
        auto& grad = op->input(1);
        auto& grad_op = grad->producer();
        // HT_LOG_INFO << "grad op " << grad_op << " placement is " << grad_op->placement();
        if (_use_current_grad_buffer) {
          // 什么都不用操作
        }
        // 不使用current_grad_buffer的话需要在这里直接将grad加到accumulate_grad_buffer上
        else {
          auto it = _reversed_grad_grad_map.find(grad->id());
          HT_ASSERT(it != _reversed_grad_grad_map.end())
            << "cannot find the mapping of " << grad << " in the reversed grad grad map";
          auto& grad_in_buffer = it->second;
          HT_ASSERT(tensor2data.find(grad->id()) != tensor2data.end());
          auto current_grad_data = tensor2data[grad->id()];
          auto accumulate_grad_data = NDArray(grad->meta(), 
                                              _accumulate_grad_buffer->AsStorage(), 
                                              _accumulate_grad_buffer->GetElementOffest(grad_in_buffer));
          auto grad_stream = grad_op->instantiation_ctx().stream(); 
          if (_grad_scale != 1) {
            NDArray::mul(current_grad_data,
                         _grad_scale,
                         grad_stream.stream_index(),
                         current_grad_data);
          }
          // 如果有一些累计梯度是switch过来的
          // 那么我们这里进行实际的sync
          auto event_it = _switch_grad_events.find(grad_in_buffer->id());
          if (event_it != _switch_grad_events.end()) {
            event_it->second->Block(grad_stream);
          } 
          NDArray::add(current_grad_data, 
                       accumulate_grad_data, 
                       grad_stream.stream_index(),
                       accumulate_grad_data);                                    
        }
        // 需要记录grad op的event来在结束时同步
        auto event = std::make_unique<hetu::impl::CUDAEvent>(grad_op->placement());
        event->Record(grad_op->instantiation_ctx().stream());
        _run_grad_events[grad->id()] = std::move(event);
        tensor2data.erase(grad); // 清除tensor2data中该grad的引用计数
        continue;
      }
      // 要进行梯度更新
      else if (_run_level == RunLevel::UPDATE) {
        // 如果有累积梯度那么此时要加上
        // 这里的逻辑和上面的正好反过来
        if (_accumulate_grad_buffer->IsAllocated()) {
          auto& grad = op->input(1);
          auto& grad_op = grad->producer();
          auto it = _reversed_grad_grad_map.find(grad->id());
          HT_ASSERT(it != _reversed_grad_grad_map.end())
            << "cannot find the mapping of " << grad << " in the reversed grad grad map";
          auto& grad_in_buffer = it->second;
          HT_ASSERT(tensor2data.find(grad->id()) != tensor2data.end());
          auto current_grad_data = tensor2data[grad->id()];
          auto accumulate_grad_data = NDArray(grad->meta(), 
                                              _accumulate_grad_buffer->AsStorage(), 
                                              _accumulate_grad_buffer->GetElementOffest(grad_in_buffer));
          auto grad_stream = Stream(grad_op->placement(),
                                    grad_op->instantiation_ctx().stream_index);
          if (_grad_scale != 1) {
            NDArray::mul(current_grad_data,
                         _grad_scale,
                         grad_stream.stream_index(),
                         current_grad_data);
          }
          // 如果有一些累计梯度是switch过来的
          // 那么我们这里进行实际的sync
          auto event_it = _switch_grad_events.find(grad_in_buffer->id());
          if (event_it != _switch_grad_events.end()) {
            event_it->second->Block(grad_stream);
          } 
          NDArray::add(current_grad_data, 
                       accumulate_grad_data, 
                       grad_stream.stream_index(),
                       current_grad_data);
          // 需要重新设置grad op的stop event来保证update算子的输入是sync的
          grad->producer()->instantiation_ctx().stop[micro_batch_id]->Record(grad_stream);
        }
      }
      // 其余情况不可能发生
      else {
        HT_RUNTIME_ERROR << "run level error";
      }
    }

    // HT_LOG_INFO << hetu::impl::comm::GetLocalDevice() << ": op execute " << op << " start...";
    // batched p2p send & recv
    // 跨hetero stage的batchedIsendIrecv已经包了一层ncclGroupStart和ncclGroupEnd
    // 但参考nccl文档可知最终取决于最外层的ncclGroupStart和ncclGroupEnd
    if ((is_pipeline_stage_send_op(op) || is_pipeline_stage_recv_op(op)) 
        && !is_shared_weight_or_grad_p2p(op)) {
      if (!is_continuous_p2p) {
        is_continuous_p2p = true;
        auto event = std::make_unique<hetu::impl::CUDAEvent>(op->placement());
        event->Record(Stream(op->placement(), kComputingStream));
        event->Block(Stream(op->placement(), kP2PStream));
        _p2p_events.emplace_back(std::move(event));
        ncclGroupStart();
        // HT_LOG_INFO << hetu::impl::comm::GetLocalDevice() << ": nccl group start";
      }
    } else if (is_continuous_p2p) {
      is_continuous_p2p = false;
      ncclGroupEnd();
      auto event = std::make_unique<hetu::impl::CUDAEvent>(op->placement());
      event->Record(Stream(op->placement(), kP2PStream));
      event->Block(Stream(op->placement(), kComputingStream));
      // event->Block(Stream(op->placement(), kOptimizerStream));
      _p2p_events.emplace_back(std::move(event));
      // HT_LOG_INFO << hetu::impl::comm::GetLocalDevice() << ": nccl group end";
    }

    // variable can be directly fetched, needn't save in tensor2data
    // AMP data transfer can be directly fetched, needn't save in tensor2data
    NDArrayList input_vals;
    input_vals.reserve(op->num_inputs());
    for (const auto& input : op->inputs()) {
      NDArray input_val;
      if (_preserved_data.find(input->id()) != _preserved_data.end()) {
        input_val = _preserved_data[input->id()];
        // 如果有一些_preserved_data是switch过来的
        // 那么我们这里进行实际的sync
        auto event_it = _switch_param_events.find(input->id());
        if (event_it != _switch_param_events.end()) {
          event_it->second->Block(op->instantiation_ctx().stream());
        }     
      } 
      // 其余情况从tensor2data中fetch
      else {
        auto it = tensor2data.find(input->id());
        HT_ASSERT(it != tensor2data.end() && it->second.is_defined())
          << "Failed to execute the \"" << op->type() << "\" operation "
          << "(with name \"" << op->name() << "\"): "
          << "Cannot find input " << input;
        auto& data = it->second;
        if (data->device() != input->placement() ||
            data->dtype() != input->dtype()) {
          tensor2data[input->id()] =
            NDArray::to(data, input->placement(), input->dtype(),
                        op->instantiation_ctx().stream_index);
        }
        input_val = tensor2data[input->id()];
        // should free memory until op aync compute complete!!!
        // recved shared weight should not be erased in first micro batch. but can be multi copied and erased in later micro batches
        if ((--tensor2degrees[input->id()]) == 0 && fetch_indices.find(input->id()) == fetch_indices.end() 
            && ((micro_batch_id == 0 && shared_weight_tensor.find(input->id()) == shared_weight_tensor.end() 
                && dtype_transfer_tensor.find(input->id()) == dtype_transfer_tensor.end())
            || micro_batch_id > 0)) {
          tensor2data.erase(input->id());
        }
      }
      input_vals.push_back(input_val);
    }
    if (is_shared_weight_or_grad_p2p(op)) {
      auto event = std::make_unique<hetu::impl::CUDAEvent>(op->placement());
      event->Record(Stream(op->placement(), kComputingStream));
      event->Block(Stream(op->placement(), kP2PStream));
      // HT_LOG_INFO << hetu::impl::comm::GetLocalDevice() << ": wte nccl group start";
      ncclGroupStart();
    }
    NDArrayList output_vals = op->Compute(input_vals, runtime_ctx, micro_batch_id);
    if (is_shared_weight_or_grad_p2p(op)) {
      // HT_LOG_INFO << hetu::impl::comm::GetLocalDevice() << ": wte nccl group end";
      ncclGroupEnd();
    }
    // HT_LOG_INFO << "micro batch[" << micro_batch_id << "] Running op " << op << " (type: " << op->type() << ") mid...";    
    // Note: The usage should be marked inside kernels, 
    // but we still mark here in case we forget to do so in some kernels. 
    NDArray::MarkUsedBy(input_vals, op->instantiation_ctx().stream());
    NDArray::MarkUsedBy(output_vals, op->instantiation_ctx().stream());
    for (size_t i = 0; i < op->num_outputs(); i++) {
      const auto& output = op->output(i);
      if (accumulated_tensor.find(output->id()) != accumulated_tensor.end()) {
        if (grad_accumulation.find(output->id()) == grad_accumulation.end()) {
          grad_accumulation[output->id()] = output_vals[i];
        } else {
          NDArray::add(grad_accumulation[output->id()], output_vals[i], 
                       op->instantiation_ctx().stream_index, grad_accumulation[output->id()]); // inplace
        }
        if (grad_accumulation_finished) {
          tensor2data[output->id()] = grad_accumulation[output->id()];
          grad_accumulation.erase(output->id()); // 清除grad_accumulation的引用计数
        }
      } else if (tensor2degrees[output->id()] > 0 || fetch_indices.find(output->id()) != fetch_indices.end()) {
        tensor2data[output->id()] = output_vals[i];
      }
    }
  // op->instantiation_ctx().stream().Sync();
  // HT_LOG_INFO << hetu::impl::comm::GetLocalDevice() << ": op execute " << op << " end...";
  }
}

void ExecutableGraph::GetExecEnvs() {
  char* env = std::getenv("HETU_STRAGGLER");
  if (env != nullptr) {
    if (std::string(env) == "ANALYSIS") {
      _straggler_flag = 1;
    } else if (std::string(env) == "EXP") {
      _straggler_flag = 2;
    } else {
      HT_RUNTIME_ERROR << "Unknown hetu straggler level: " + std::string(env);
    }
  } else {
    // 默认不分析straggler
    _straggler_flag = 0;
  }

  env = std::getenv("HETU_STRAGGLER_LOG_FILE");
  if (env != nullptr) {
    _straggler_log_file_path = std::string(env);
  } else {
    // 默认不对straggler打log
    _straggler_log_file_path = "";
  }

  env = std::getenv("HETU_MEMORY_PROFILE");
  if (env != nullptr) {
    if (std::string(env) == "MICRO_BATCH") {
      _memory_profile_level = MEMORY_PROFILE_LEVEL::MICRO_BATCH;
      _all_micro_batches_memory_info.clear();
    } else if (std::string(env) == "INFO") {
      _memory_profile_level = MEMORY_PROFILE_LEVEL::INFO;
    } else if (std::string(env) == "WARN") {
      _memory_profile_level = MEMORY_PROFILE_LEVEL::WARN;
    } else {
      HT_RUNTIME_ERROR << "Unknown hetu memory profile level: " + std::string(env);
    }
  } else {
    // 默认不profile
    _memory_profile_level = MEMORY_PROFILE_LEVEL::WARN;
  }

  env = std::getenv("HETU_MEMORY_LOG_FILE");
  if (env != nullptr) {
    _memory_log_file_path = std::string(env);
  } else {
    // 默认不对memory打log
    _memory_log_file_path = "";
  }
}

NDArrayList ExecutableGraph::Run(const Tensor& loss, const TensorList& fetches, 
                                 const FeedDict& feed_dict, const int num_micro_batches,
                                 const int cur_strategy_id, RunLevel run_level, const double grad_scale) {
  
  GetExecEnvs();
  TIK(run);
  _grad_scale = grad_scale;
  auto& local_device = hetu::impl::comm::GetLocalDevice();
  HT_LOG_DEBUG << local_device << ": exec graph run begin .............";
  if (_memory_profile_level == MEMORY_PROFILE_LEVEL::INFO)
    GetCUDAProfiler(local_device)->PrintCurrMemoryInfo(name() + " run begin");

  // TODO: For each pair of `fetches` and `feed_dict`,
  // deduce the optimal execution plan, and cache it.
  _num_micro_batches = num_micro_batches;
  HT_LOG_DEBUG << local_device << ": 0. Create Execution Plan [begin]";
  auto is_op_computed = [&](const Operator& op) -> bool {
    return Operator::all_output_tensors_of(op, [&](const Tensor& tensor) {
      return feed_dict.find(tensor->id()) != feed_dict.end();
    });
  };

  bool is_execute_plan_changed = false;
  for (auto& fetch : fetches) {
    if (!fetch->has_placement_group() || 
        (fetch->placement_group_union().has(local_device) && 
         fetch->placement().is_undetermined())) {
      // instantiate ops
      HT_LOG_DEBUG << local_device << ": [Execution Plan] Instantiate begin...";
      Instantiate(fetches, local_device);
      HT_LOG_DEBUG << local_device << ": [Execution Plan] Instantiate end...";

      // init topo contains comm_op
      OpRefList topo = Graph::TopoSort(fetches, num_ops(), is_op_computed);
      HT_LOG_DEBUG << local_device << ": global topo before substitute comm_op: " << topo;

      // substitute comm_op
      HT_LOG_DEBUG << local_device << ": [Execution Plan] substitute comm_op begin...";
      Graph::push_graph_ctx(id()); // ensure the new ops created in execute_graph
      SubstituteCommOp(topo);
      Graph::pop_graph_ctx();
      HT_LOG_DEBUG << local_device << ": [Execution Plan] substitute comm_op end...";

      // update topo with substituted comm_ops
      OpRefList updated_topo = Graph::TopoSort(fetches, num_ops(), is_op_computed);
      HT_LOG_DEBUG << local_device << ": global topo before add contiguous op: " << topo;

      // insert contiguous ops
      HT_LOG_DEBUG << local_device << ": [Execution Plan] insert contiguous op begin...";
      Graph::push_graph_ctx(id()); // ensure the new ops created in execute_graph
      InsertContiguousOp(updated_topo);
      Graph::pop_graph_ctx();
      HT_LOG_DEBUG << local_device << ": [Execution Plan] insert contiguous op end...";
      is_execute_plan_changed = true;
      break;
    }
  }

  if (is_execute_plan_changed) {
    // TODO: replace the fetches to the new substitued results after SubstituteCommOp
    for (auto& fetch : fetches) {
      auto& fetch_op = fetch->producer();
      HT_ASSERT(!is_comm_op(fetch_op)) << fetch << ": is substitued already, don't try to fetch it.";
    }

    // execute in each iteration, should be cached 
    HT_LOG_DEBUG << local_device << ": [Execution Plan] get local fw/bw topo begin...";
    // update topo
    OpRefList updated_topo = Graph::TopoSort(fetches, -1, is_op_computed);
    // HT_LOG_DEBUG << local_device << ": updated global topo after substitute comm_op: " << updated_topo;

    // split into fw_topo and bw_topo
    OpRefList fw_topo, bw_topo;
    std::tie(fw_topo, bw_topo) = disentangle_forward_and_backward_ops_by_loss(updated_topo, {loss});
    // OpRefList fw_topo, bw_topo;
    // std::tie(fw_topo, bw_topo) = disentangle_forward_and_backward_ops(updated_topo);

    // judge whether is shared weight p2p in fw/bw.
    auto is_fw_share_weight_p2p_send = [&](const OpRef& op_ref) -> bool {
      // HT_LOG_WARN << "call is_fw_share_weight_p2p_send";
      if (is_pipeline_stage_send_op(op_ref.get())) {
        Operator input_op = op_ref.get()->input(0)->producer();
        while (true) {
          if (is_slice_op(input_op)) {
            input_op = input_op->input(0)->producer();
            continue;
          }
          if (is_variable_op(input_op) 
              || (is_data_transfer_op(input_op) && is_variable_op(input_op->input(0)->producer()))) {
            return true;
          }
          break;
        }
      }
      return false;
    };
    auto is_fw_share_weight_p2p_recv = [&](const OpRef& op_ref) -> bool {
      // HT_LOG_WARN << "call is_fw_share_weight_p2p_recv";
      if (is_pipeline_stage_recv_op(op_ref.get())) {
        Operator input_op = op_ref.get()->in_dep_linker(0)->producer();
        if (is_variable_op(input_op) 
            || (is_data_transfer_op(input_op) && is_variable_op(input_op->input(0)->producer()))) {
          // HT_LOG_INFO << local_device << ": shared weight p2p fw recv: " << op_ref;
          return true;
        }
      }
      return false;
    };
    auto is_bw_share_weight_grad_p2p_send = [&](const OpRef& op_ref) -> bool {
      // HT_LOG_WARN << "call is_bw_share_weight_p2p_send";
      if (is_pipeline_stage_send_op(op_ref.get())) {
        Operator output_op = op_ref.get()->out_dep_linker()->consumer(0);
        if (is_sum_op(output_op)) {
          auto& sum_op = output_op;
          if (is_optimizer_update_op(sum_op->output(0)->consumer(0))) {
            return true;
          } 
          if (is_comm_op(sum_op->output(0)->consumer(0))) {
            auto& comm_op = sum_op->output(0)->consumer(0);
            auto preferred_device = GetPrevStage().get(0);
            auto comm_type = dynamic_cast<CommOpImpl&>(comm_op->body()).get_comm_type(comm_op, preferred_device);
            if ((comm_type == ALL_REDUCE_OP || comm_type == SPLIT_ALL_REDUCE_OP
                || comm_type == REDUCE_SCATTER_OP || comm_type == SPLIT_REDUCE_SCATTER_OP) 
                && is_optimizer_update_op(comm_op->output(0)->consumer(0))) {
              return true;
            }
          }
        }
      }
      return false;    
    };
    auto is_bw_share_weight_grad_p2p_recv = [&](const OpRef& op_ref) -> bool {
      // HT_LOG_WARN << "call is_bw_share_weight_p2p_recv";
      if (is_pipeline_stage_recv_op(op_ref.get())) {
        auto output_op = op_ref.get()->output(0)->consumer(0);
        while (true) {
          if (is_concat_op(output_op)) {
            output_op = output_op->output(0)->consumer(0);
            continue;
          }
          if (is_sum_op(output_op)) {
            auto& sum_op = output_op;
            if (is_optimizer_update_op(sum_op->output(0)->consumer(0))) {
              return true;
            } 
            for (auto& consumer_op : sum_op->output(0)->consumers()) {
              if (is_grad_reduce_op(consumer_op)) {
                if (is_optimizer_update_op(consumer_op.get()->output(0)->consumer(0))) {
                  // HT_LOG_INFO << local_device << ": shared weight p2p bw recv: " << op_ref;
                  return true;
                }
              }
            }
          }
          break;
        }
      }
      return false;
    };

    // get local_fw_topo and local_bw_topo, not contains placeholder & varivale ops
    // ops to substitute comm_op is in the same placement_group, but in the different placement
    OpRefList local_fw_topo, local_bw_topo, local_placeholder_variable_ops, local_topo;
    auto get_local_topo = [&](OpRefList& _topo, OpRefList& _local_topo, OpRefList& _placeholder_variable_ops) {
      // move p2p send op to topo tail
      OpRefList send_op_list;
      OpRefList recv_op_list;
      OpRefList compute_op_list;
      OpRefList update_op_list;
      OpRefList final_update_op_list;
      OpRefList share_weight_recv_op_list;
      OpRefList share_weight_grad_recv_op_list;
      // todo: assume pp stages = [0,1,2,3]->[4,5,6,7], then 0 send pre-half of wte to 4, 1 send last-half of wte to 5; 
      // 2 send pre-half of wte to 6, 3 send last-half of wte to 7; notice that 0 and 2 are send the same, 1 and 3 are send the same
      // so 0 can send half of pre-half to 4, 2 can send another half of pre-half to 6, then 4 and 6 do gather(at this time, 4 and 6
      // are waiting for pp bubbles, the time will be reused)
      // todo2: in pipeline last micro batch, stage id > 0 can move grad_reduce & update & group after pipeline p2p and use bubble
      // to do later update, but stage id = 0 can do aync grad_reduce immediately after weight grad was computed, which can be 
      // overlapped with backward compute(no overhead for pure dp, but may make tp backward allreduce slower)
      for (auto& op_ref : _topo) {
        if (op_ref.get()->placement() == local_device || op_ref.get()->op_meta().is_step) {
          // share weight p2p send op will not block anything! so treat it as commom compute op
          // fw weight share only in micro batch 0, bw weight grad share only in last micro batch
          // HT_LOG_DEBUG << "get op type for " << op_ref.get();
          if (is_fw_share_weight_p2p_send(op_ref) || is_bw_share_weight_grad_p2p_send(op_ref)) {
            compute_op_list.push_back(op_ref);
            // HT_LOG_WARN << "compute_op";
          } else if (is_fw_share_weight_p2p_recv(op_ref)) {
            share_weight_recv_op_list.push_back(op_ref);
            // HT_LOG_WARN << "share_weight_recv_op";
          } else if (is_bw_share_weight_grad_p2p_recv(op_ref)) {
            share_weight_grad_recv_op_list.push_back(op_ref);
            // HT_LOG_WARN << "share_weight_grad_recv_op";
          } else if (is_pipeline_stage_send_op(op_ref.get())) {          
            send_op_list.push_back(op_ref);
            // HT_LOG_WARN << "send_op";
          } else if (is_pipeline_stage_recv_op(op_ref.get())) {
            recv_op_list.push_back(op_ref);
            // HT_LOG_WARN << "recv_op";
          } else {
            if (is_placeholder_op(op_ref) || is_variable_op(op_ref)) {
              _placeholder_variable_ops.push_back(op_ref);
            } else if (is_grad_reduce_op(op_ref)
                       && is_grad_reduce_op(op_ref.get()->output(0)->consumer(0))
                       && is_optimizer_update_op(op_ref.get()->output(0)->consumer(0)->output(0)->consumer(0))) {
              update_op_list.push_back(op_ref);
            } else if (is_grad_reduce_op(op_ref) 
                       && is_optimizer_update_op(op_ref.get()->output(0)->consumer(0))) {
              update_op_list.push_back(op_ref);
            } else if (is_optimizer_update_op(op_ref)) {
              final_update_op_list.push_back(op_ref);
            } else if (is_group_op(op_ref)) {
              final_update_op_list.push_back(op_ref);
            } else {
              compute_op_list.push_back(op_ref);
            }
          }
        }
      }
      _local_topo.insert(_local_topo.end(), share_weight_grad_recv_op_list.begin(), share_weight_grad_recv_op_list.end()); // first stage
      _local_topo.insert(_local_topo.end(), share_weight_recv_op_list.begin(), share_weight_recv_op_list.end()); // last stage
      _local_topo.insert(_local_topo.end(), recv_op_list.begin(), recv_op_list.end());
      _local_topo.insert(_local_topo.end(), compute_op_list.begin(), compute_op_list.end());
      _local_topo.insert(_local_topo.end(), send_op_list.begin(), send_op_list.end());
      // move allreduce/reduce-scatter & udpate & group op after pipeline p2p, to make p2p & allreduce/reduce-scatter overlap
      _local_topo.insert(_local_topo.end(), update_op_list.begin(), update_op_list.end());
      _local_topo.insert(_local_topo.end(), final_update_op_list.begin(), final_update_op_list.end());
    };
    get_local_topo(fw_topo, local_fw_topo, local_placeholder_variable_ops);
    get_local_topo(bw_topo, local_bw_topo, local_placeholder_variable_ops); 

    local_topo.reserve(local_placeholder_variable_ops.size() + local_fw_topo.size() + local_bw_topo.size());
    local_topo.insert(local_topo.end(), local_placeholder_variable_ops.begin(), local_placeholder_variable_ops.end());
    local_topo.insert(local_topo.end(), local_fw_topo.begin(), local_fw_topo.end());
    local_topo.insert(local_topo.end(), local_bw_topo.begin(), local_bw_topo.end());
    HT_LOG_DEBUG << local_device  << ": local placeholder & variable ops: " << local_placeholder_variable_ops
                 << "\nlocal fw topo: " << local_fw_topo << "\nlocal bw topo: " << local_bw_topo;
    HT_LOG_DEBUG << local_device << ": [Execution Plan] get local fw/bw topo end...";

    HT_LOG_DEBUG << local_device << ": [Execution Plan] get grad to grad map begin...";
    for (auto& op_ref : local_bw_topo) {
      if (is_optimizer_update_op(op_ref)) {
        auto& param = op_ref.get()->input(0);
        auto& grad = op_ref.get()->input(1);
        auto it = _grad_map.find(param->id());
        HT_ASSERT(it != _grad_map.end())
          << "cannot find the mapping of " << param << " in the grad map";
        auto& grad_in_buffer = it->second;
        HT_ASSERT(grad_in_buffer->meta() == grad->meta())
          << "the meta of the grad before/after substitute comm op should be equal"
          << ", but meta of grad in buffer is " << grad_in_buffer->meta()
          << ", and meta of grad is " << grad->meta();
        HT_ASSERT(grad_in_buffer->cur_ds_union().check_equal(grad->cur_ds_union()))
          << "the distributed states of the grad before/after substitute comm op should be equal";
        HT_ASSERT(grad_in_buffer->producer()->device_group_union().check_equal(grad->placement_group_union()))
          << "the device group of the grad before/after substitute comm op should be equal";
        _grad_grad_map[grad_in_buffer->id()] = grad;
        _reversed_grad_grad_map[grad->id()] = grad_in_buffer;
      }
    }
    HT_LOG_DEBUG << local_device << ": [Execution Plan] get grad to grad map end...";

    HT_LOG_DEBUG << local_device << ": [Execution Plan] get shared weights & dtype transfered weights begin...";
    // todo: get all shared variable op related (send, recv), cached in first micro batch, and used in later micro batches 
    TensorIdSet shared_weight_tensor;
    OpIdSet shared_weight_p2p;
    TensorIdSet dtype_transfer_tensor;
    // (group1) variable op -> send -> (group2) recv -> other ops
    for (auto& op_ref : local_fw_topo) {
      if (is_fw_share_weight_p2p_send(op_ref)) {
        shared_weight_p2p.insert(op_ref.get()->id());
      }
      if (is_fw_share_weight_p2p_recv(op_ref)) {
        shared_weight_p2p.insert(op_ref.get()->id());
        shared_weight_tensor.insert(op_ref.get()->output(0)->id());
      }
      if (is_data_transfer_op(op_ref) && is_variable_op(op_ref.get()->input(0)->producer())) {
        dtype_transfer_tensor.insert(op_ref.get()->output(0)->id());
      }
    }
    OpIdSet shared_weight_grad_p2p;
    for (auto& op_ref : local_bw_topo) {
      if (is_bw_share_weight_grad_p2p_send(op_ref) || is_bw_share_weight_grad_p2p_recv(op_ref)) {
        shared_weight_grad_p2p.insert(op_ref.get()->id());
      }
      if (is_data_transfer_op(op_ref) && is_variable_op(op_ref.get()->input(0)->producer())) {
        dtype_transfer_tensor.insert(op_ref.get()->output(0)->id());
      }
    }
    HT_LOG_DEBUG << local_device << ": [Execution Plan] get shared weights & dtype transfered weights end...";

    HT_LOG_DEBUG << local_device << ": [Execution Plan] get accumulated tensor & ops begin...";
    // some special ops shouldn't be updated before grad accumulation finished
    TensorIdSet accumulated_tensor;
    OpDeque accumulated_ops_deque;
    for (auto& op_ref : local_bw_topo) {
      auto& op = op_ref.get();
      // HT_LOG_INFO << "handling " << op;
      // update op placement group = variable op placement group
      // care about the placement group binding rules based on fw_op_id in autograd code (graph.cc)
      // grad_reduce = allreduce or reduce-scatter
      // 1. compute_op -> (sum_op) -> update_op (local_group)
      // 2. compute_op -> grad_reduce -> update_op (local_group)
      // 3. compute_op -> sum_op -> grad_reduce -> update_op (local_group)
      // 4. compute_op -> pipeline_send (group1)  pipeline_recv -> update_op (group2)
      // 5. compute_op -> grad_reduce -> pipeline_send (group1)  pipeline_recv -> update_op (group2)
      // 6. compute_op -> pipeline_send (group1)  pipeline_recv -> sum_op -> (grad_reduce) -> update_op (group2)

      // 注意：有sum op的情况下，如果不是对sum op的output做accumulation，
      // 那么请务必把sum op的所有除了p2p recv的inputs都标注为accumulated_tensor!!!
      // local group or group2 cases (1,2,3,4,5,6)
      if (is_optimizer_update_op(op)) {
        Tensor& grad = op->input(1);
        Operator& grad_op = grad->producer();
        if (is_grad_reduce_op(grad_op) || is_sum_op(grad_op)) {
          // case 6: for sum op recv input 
          bool is_weight_share_case = false;
          TensorList sum_inputs_except_recv;
          // share weight without dp
          if (is_sum_op(grad_op)) {
            for (auto& sum_input : grad_op->inputs()) {
              if (is_fused_pipeline_stage_recv_op(sum_input->producer())) {
                accumulated_ops_deque.emplace_back(get_last_pipeline_stage_recv_op(sum_input->producer()));
                is_weight_share_case = true;
              } else {
                sum_inputs_except_recv.push_back(sum_input);
              }
            }
          }
          // share weight with dp
          if (is_grad_reduce_op(grad_op) && is_sum_op(grad_op->input(0)->producer())) {
            /*
            HT_LOG_INFO << "share weight with dp, need to first sum "
              << grad_op->input(0)->producer()->inputs() << " and then reduce";
            */
            for (auto& sum_input : grad_op->input(0)->producer()->inputs()) {
              if (is_fused_pipeline_stage_recv_op(sum_input->producer())) {
                accumulated_ops_deque.emplace_back(get_last_pipeline_stage_recv_op(sum_input->producer()));
                is_weight_share_case = true;
              } else {
                sum_inputs_except_recv.push_back(sum_input);
              }
            }
          }
          // case 6: for sum op inputs except recv
          if (is_weight_share_case) {
            for (auto& sum_input : sum_inputs_except_recv) {
              accumulated_tensor.insert(sum_input->id());
            }
          }
          // case 2, 3 or (case 1 with sum)
          if (!is_weight_share_case) {
            if (is_grad_reduce_op(grad_op)) {
              if (is_grad_reduce_op(grad_op->input(0)->producer())) {
                accumulated_tensor.insert(grad_op->input(0)->producer()->input(0)->id());
                accumulated_ops_deque.push_back(grad_op->input(0)->producer());
              } else {
                accumulated_tensor.insert(grad_op->input(0)->id());
              }
              accumulated_ops_deque.push_back(grad_op);
            } else if (is_sum_op(grad_op)) { // examples: shared wte
              accumulated_tensor.insert(grad->id());
              accumulated_ops_deque.push_back(op);
            }
          }
        } 
        // case 4, 5
        else if (is_fused_pipeline_stage_recv_op(grad_op)) {
          accumulated_ops_deque.push_back(get_last_pipeline_stage_recv_op(grad_op));
        } 
        // case 1
        else {
          accumulated_tensor.insert(grad->id());
          accumulated_ops_deque.push_back(op);
        }
      } 
      // group1 cases (4,5,6)
      else if (is_pipeline_stage_send_op(op)) {
        for (auto& consumer_op : op->out_dep_linker()->consumers()) {
          // case 4,5
          if (is_optimizer_update_op(consumer_op)) {
            Tensor& grad = op->input(0);
            Operator& grad_op = grad->producer();
            if (is_grad_reduce_op(grad_op)) {
              if (is_grad_reduce_op(grad_op->input(0)->producer())) {
                accumulated_tensor.insert(grad_op->input(0)->producer()->input(0)->id());
                accumulated_ops_deque.push_back(grad_op->input(0)->producer());
              } else {
                accumulated_tensor.insert(grad_op->input(0)->id());
              }
              accumulated_ops_deque.push_back(grad_op);
            } else {
              accumulated_tensor.insert(grad->id());
              accumulated_ops_deque.push_back(op);
            }
          } 
          // case 6
          else if (is_sum_op(consumer_op)) {
            Operator& sum_op = consumer_op.get();
            // share weight without dp
            if (is_optimizer_update_op(sum_op->output(0)->consumer(0))) {
              accumulated_tensor.insert(op->input(0)->id());
              accumulated_ops_deque.push_back(op);
            }
            // share weight with dp
            if (is_comm_op(sum_op->output(0)->consumer(0))) {
              Operator& comm_op = sum_op->output(0)->consumer(0);
              auto preferred_device = GetPrevStage().get(0);
              auto comm_type = dynamic_cast<CommOpImpl&>(comm_op->body()).get_comm_type(comm_op, preferred_device);
              if ((comm_type == ALL_REDUCE_OP || comm_type == SPLIT_ALL_REDUCE_OP
                  || comm_type == REDUCE_SCATTER_OP || comm_type == SPLIT_REDUCE_SCATTER_OP) 
                  && is_optimizer_update_op(comm_op->output(0)->consumer(0))) {
                accumulated_tensor.insert(op->input(0)->id());
                accumulated_ops_deque.push_back(op);
              }
            }
          }
        }
      }
    }
    // HT_LOG_INFO << "try to get accumulated ops";
    OpIdSet accumulated_ops;
    while (!accumulated_ops_deque.empty()) {
      auto& op = accumulated_ops_deque.front();
      accumulated_ops_deque.pop_front();
      accumulated_ops.insert(op->id());
      Operator::for_each_output_tensor(op, [&](const Tensor& output) {
        for (auto& consumer_op : output->consumers()) {
          if (consumer_op.get()->placement() == local_device) {
            accumulated_ops_deque.push_back(consumer_op.get());
          }
        }
      });
    }
    // HT_LOG_INFO << local_device << ": accumulated ops: " << accumulated_ops << "\nlocal_bw_topo: " << local_bw_topo;
    HT_LOG_DEBUG << local_device << ": [Execution Plan] get accumulated tensor & ops end...";
    // update & cached execute plan 
    _execute_plan.update(local_placeholder_variable_ops, local_fw_topo, local_bw_topo, local_topo, dtype_transfer_tensor,
                         shared_weight_tensor, shared_weight_p2p, shared_weight_grad_p2p, accumulated_tensor, accumulated_ops);
    if (_used_ranks.size() >= 2) {
      auto& mpi_comm_group = hetu::impl::comm::MPICommunicationGroup::GetOrCreate(_used_ranks);
      mpi_comm_group->Barrier(true);
    }
  }
  TOK(run);
  HT_LOG_DEBUG << local_device << ": prepare execution plan cost time = " << COST_MSEC(run) << " ms."; 

  // calculate params
  bool is_calculate_params = false;
  if (is_calculate_params) {
    int64_t params_size = 0;
    for (auto& op : _execute_plan.local_topo) {
      if (is_variable_op(op)) {
        params_size += op.get()->output(0)->numel();
        // HT_LOG_INFO << local_device << ": variable op " << op << ", shape = " << op.get()->output(0)->shape();
      }
    }
    HT_LOG_INFO << local_device << ": params_size = " << params_size;
  }

  HT_LOG_DEBUG << local_device << ": 0. Create Execution Plan [end]";

  // ********************** Run Level Check Point **********************
  if (_run_level == RunLevel::TOPO) {
    return {};
  }
  // ********************** Run Level Check Point **********************

  HT_LOG_DEBUG << local_device << ": 1. pipeline init[begin]";
  // pipeline compute
  // runtimectx for m micro batches
  std::vector<RuntimeContext> runtime_ctx_list(num_micro_batches, 
    RuntimeContext(_execute_plan.local_topo.size(), _shape_plan_pool.at(_active_shape_plan)));
  // tensor data for m micro batches
  std::vector<Tensor2NDArrayMap> tensor2data_list(num_micro_batches);
  // tensor degrees for m micro batches, if degree=0 && not in fetches, free memory for this tensor
  std::vector<Tensor2IntMap> tensor2degrees_list(num_micro_batches);
  // flush update once for m micro batches
  Tensor2NDArrayMap grad_accumulation;

  // placeholder ops: get feed in dict & split into m micro batches
  for (const auto& kv : feed_dict) {
    if (!kv.second.is_defined()) continue; // only feed placeholder_op in local device group
    auto micro_batches = NDArray::split(kv.second, num_micro_batches);
    // 加一个pipeline split的tensor状态
    for (int i = 0; i < num_micro_batches; i++) {
      // tensor2data_list[i][kv.first] = NDArray::squeeze(micro_batches[i], 0);
      tensor2data_list[i][kv.first] = micro_batches[i];
    }
  }
  std::unordered_map<TensorId, size_t> fetch_indices;
  for (size_t i = 0; i < fetches.size(); i++)
    fetch_indices[fetches.at(i)->id()] = i;
  // get consume times for each tensor
  Tensor2IntMap tensor2degrees;
  for (auto& op_ref : _execute_plan.local_topo) {
    for (auto& input : op_ref.get()->inputs()) {
      tensor2degrees[input->id()]++;
    }
  }
  for (int i = 0; i < num_micro_batches; i++) {
    tensor2degrees_list[i] = tensor2degrees;
  }
  HT_LOG_DEBUG << local_device << ": 1. pipeline init[end]";

  HT_LOG_DEBUG << local_device << ": 2. alloc and compute buffer[begin]";
  // alloc origin/transfer params and pre-compute, alloc grads
  AllocRuntimeBuffer(runtime_ctx_list);
  HT_LOG_DEBUG << local_device << ": 2. alloc and compute buffer[end]";

  // ********************** Run Level Check Point **********************
  if (_run_level == RunLevel::ALLOC) {
    SynchronizeAllStreams();
    // memory debug use
    // hetu::impl::comm::EmptyNCCLCache();
    if (_memory_profile_level == MEMORY_PROFILE_LEVEL::INFO)
      GetCUDAProfiler(local_device)->PrintCurrMemoryInfo(name() + " run ALLOC end");
    return {};
  }
  // ********************** Run Level Check Point **********************

  HT_LOG_DEBUG << local_device << ": 3. compute[begin]";
  if (_pipeline_map.find(local_device) == _pipeline_map.end()) {
    HT_LOG_WARN << local_device << ": can't figure out which pipeline the local device belongs to"
      << ", so we just return";
    return {};
  }
  auto& pipeline = _pipeline_map[local_device];
  int num_stages = pipeline.size();
  bool is_inference = (_execute_plan.local_bw_topo.size() == 0);
  HT_LOG_DEBUG << local_device << ": num_stages = " << num_stages << ", stages = " << pipeline 
    << ", num_micro_batches = " << num_micro_batches << ", is_inference = " << is_inference;
  // get task schedule table for pipedream-flush, also suitable for non-pipeline cases
  auto schedule = GeneratePipedreamFlushSchedule(
    num_stages, num_micro_batches, is_inference);
  // // get task schedule table for gpipe    
  // auto schedule = generate_gpipe_schedule(num_stages, num_micro_batches);
  // get tasks for current stage
  // int stage_id = local_device.index() / _stages.at(0).num_devices();
  int stage_id = -1;
  for (int i = 0; i < pipeline.size(); i++) {
    if (pipeline[i].contains(local_device)) {
      stage_id = i;
    }
  }
  // HT_LOG_DEBUG << local_device << ": stages = " << _stages << "; stage id = " << stage_id;
  auto& tasks = schedule[stage_id];
  HT_LOG_DEBUG << local_device << ": stage id = " << stage_id;
  bool is_continuous_p2p = false;
  for (size_t i = 0; i < tasks.size(); i++) {
    auto& task = tasks[i];
    bool is_forward = task.first;
    size_t& micro_batch_id = task.second;
    auto& tensor2data = tensor2data_list[micro_batch_id];
    auto& tensor2degrees = tensor2degrees_list[micro_batch_id];
    auto& runtime_ctx = runtime_ctx_list[micro_batch_id];
    // micro batch i>0 reuse: 
    // 0. shared weight which was recved in micro batch 0
    // 1. f32 -> fp16, bf16 weight which was transfered in micro batch 0
    if (micro_batch_id > 0) {
      if (!_execute_plan.shared_weight_tensor.empty()) {
        for (auto& shared_weight_id : _execute_plan.shared_weight_tensor) {
          if (tensor2data.find(shared_weight_id) != tensor2data.end()) break; // avoid assign twice by fw, bw
          tensor2data[shared_weight_id] = tensor2data_list[0][shared_weight_id];
        }
      }
      if (!_execute_plan.dtype_transfer_tensor.empty()) {
        for (auto& dtype_transfer_id : _execute_plan.dtype_transfer_tensor) {
          if (tensor2data.find(dtype_transfer_id) != tensor2data.end()) break; // avoid assign twice by fw, bw
          tensor2data[dtype_transfer_id] = tensor2data_list[0][dtype_transfer_id];
        }
      }
    }
    if (is_forward) {
      HT_LOG_DEBUG << local_device << ": [micro batch " << micro_batch_id << ": forward begin]";
    } else {
      HT_LOG_DEBUG << local_device << ": [micro batch " << micro_batch_id << ": backward begin]";
    }
    // micro batch i: profile memory begin
    if (_memory_profile_level == MEMORY_PROFILE_LEVEL::MICRO_BATCH) {
      auto micro_batch_memory_info = std::make_shared<MicroBatchMemoryInfo>();
      micro_batch_memory_info->is_forward = is_forward;
      micro_batch_memory_info->stage_id = stage_id;
      micro_batch_memory_info->micro_batch_id = micro_batch_id;
      micro_batch_memory_info->begin_memory_info = GetCUDAProfiler(local_device)->GetCurrMemoryInfo();
      _all_micro_batches_memory_info.emplace_back(micro_batch_memory_info);
    }
    // micro batch i: execute fw/bw
    if (is_forward) {
      ComputeFunc(micro_batch_id, _execute_plan.local_fw_topo, runtime_ctx,
                  tensor2data, tensor2degrees, grad_accumulation, false, 
                  feed_dict, fetches, fetch_indices, is_continuous_p2p);
    } else {
      bool grad_accumulation_finished = (i == tasks.size() - 1);
      ComputeFunc(micro_batch_id, _execute_plan.local_bw_topo, runtime_ctx, 
                  tensor2data, tensor2degrees, grad_accumulation, grad_accumulation_finished, 
                  feed_dict, fetches, fetch_indices, is_continuous_p2p);
    }
    // micro batch i: profile memory end
    if (_memory_profile_level == MEMORY_PROFILE_LEVEL::MICRO_BATCH) {
      _all_micro_batches_memory_info.back()->end_memory_info = GetCUDAProfiler(local_device)->GetCurrMemoryInfo();
      // HT_LOG_INFO << *_all_micro_batches_memory_info.back();
    }
    if (is_forward) {
      HT_LOG_DEBUG << local_device << ": [micro batch " << micro_batch_id << ": forward end]";
    } else {
      HT_LOG_DEBUG << local_device << ": [micro batch " << micro_batch_id << ": backward end]";
    }
  }
  if (is_continuous_p2p) {
    ncclGroupEnd();
    auto event = std::make_unique<hetu::impl::CUDAEvent>(local_device);
    event->Record(Stream(local_device, kP2PStream));
    event->Block(Stream(local_device, kComputingStream));
    // event->Block(Stream(local_device, kOptimizerStream));
    _p2p_events.emplace_back(std::move(event));
  }
  HT_LOG_DEBUG << local_device << ": 3. compute[end]";

  // ********************** Run Level Check Point **********************
  // 仅仅是算出grad但不更新
  // 这里需要先对grad op进行sync
  if (_run_level == RunLevel::GRAD) {
    // 理论上这里我们可以不让_run_grad_events同步
    // 假如之后切换到别的exec graph的话再在切换grad的时候再进行同步
    for (const auto& event_it : _run_grad_events) {
      event_it.second->Sync();
    }
    _run_grad_events.clear();
    // Question: 可能单独设计接口指定dst exec graph去切换能更快更省显存
    // 即，当前current_grad_buffer切换到dst exec graph后再加到accumulate_grad_buffer上
    // 但dp8逐渐切换到tp8的例子里，逐一切换和直接切换到dst并无明显区别
    // 因此目前grad也用两两间的热切换来弄
    if (_use_current_grad_buffer) {
      // 在define graph中自动切换accumulate_grad_buffer
      // 然后将当前的current_grad_buffer加到当前的accumulate_grad_buffer后清空即可
      if (!_current_grad_buffer->IsEmpty() && !_accumulate_grad_buffer->IsEmpty()) {
        if (!_accumulate_grad_buffer->IsAllocated()) {
          // 说明是第一次算grad，之前没有累积grad
          // 直接bind即可
          _accumulate_grad_buffer->Bind(_current_grad_buffer->AsStorage());
        } else {
          // 用kBlockingStream集中对整个buffer进行一次add
          // 相比于算出来某一个grad后进行局部的async的add
          // 虽然并发程度降低，但是写法上会简单许多
          auto current_grad_buffer_data = _current_grad_buffer->AsNDArray();
          auto accumulate_grad_buffer_data = _accumulate_grad_buffer->AsNDArray();
          if (_grad_scale != 1) {
            NDArray::mul(current_grad_buffer_data,
                         _grad_scale,
                         kBlockingStream,
                         current_grad_buffer_data);
          }
          // 如果有一些累计梯度是switch过来的
          // 那么我们这里进行实际的sync
          for(const auto& event_it : _switch_grad_events) {
            event_it.second->Sync();
          } 
          // 当前的计算的梯度也需要sync
          for(const auto& event_it : _run_grad_events) {
            event_it.second->Sync();
          } 
          NDArray::add(current_grad_buffer_data, 
                       accumulate_grad_buffer_data, 
                       kBlockingStream,
                       accumulate_grad_buffer_data);
        }
        // 释放当前grad
        // 2024.3.3 update
        // 把current grad buffer的清理放在需要热切换的时候
        // _current_grad_buffer->Free();
      }
    } 
    // 为节省显存峰值，可以不使用current_grad_buffer
    else {
      // 什么都不用操作
      // 已经在ComputeFunc中将grad加到了accumulate_grad_buffer中
    }
    _p2p_events.clear();
    if (_memory_profile_level == MEMORY_PROFILE_LEVEL::INFO)
      GetCUDAProfiler(local_device)->PrintCurrMemoryInfo(name() + " run GRAD end");
    return {};
  }
  // 说明是RunLevel::UPDATE了
  // 提前进行一些固有map的清空（sync结果前）
  // 这样CPU和GPU可以异步进行
  _run_grad_events.clear();
  if (!_transfer_param_buffer->IsEmpty()) {
    HT_ASSERT(_transfer_param_buffer->IsAllocated()) 
      << "transfer param buffer should be allocated";
    for (auto& op_ref : _execute_plan.local_placeholder_variable_ops) {
      auto& op = op_ref.get();
      if (is_variable_op(op) && _parameter_ops.find(op->id()) != _parameter_ops.end()) {
        auto it = _transfer_map.find(op->output(0)->id());
        HT_ASSERT(it != _transfer_map.end())
          << "The transfer map does not consist of " << op->output(0);
        auto& transfer_param = it->second;
        auto data_it = _preserved_data.find(transfer_param->id());
        HT_ASSERT(data_it != _preserved_data.end())
          << "The preserved data does not consist of " << transfer_param;
        _preserved_data.erase(data_it);
      }
    }
    // _transfer_param_buffer->Free();
  }
  // ********************** Run Level Check Point **********************

  HT_LOG_DEBUG << local_device << ": 4. get results[begin]";
  NDArrayList results(fetches.size(), NDArray());
  std::unordered_set<OpId> to_sync_op_ids;
  to_sync_op_ids.reserve(fetches.size());
  for (auto& op_ref : _execute_plan.local_topo) {
    auto& op = op_ref.get();
    Operator::for_each_output_tensor(op, [&](const Tensor& output) {
      auto it = fetch_indices.find(output->id());
      if (it != fetch_indices.end()) {
        if (output->output_id() >= 0) {
          if (is_variable_op(op) || _execute_plan.accumulated_ops.find(op) != _execute_plan.accumulated_ops.end() 
            || _execute_plan.accumulated_tensor.find(output->id()) != _execute_plan.accumulated_tensor.end()) {
            results[it->second] = tensor2data_list[num_micro_batches - 1][output->id()];
          } else if (is_placeholder_op(op)) {
            auto feed_it = feed_dict.find(output->id());
            if (feed_it != feed_dict.end()) {
              results[it->second] = feed_it->second;
            }
          } else {
            NDArrayList result;
            result.reserve(num_micro_batches);
            for (auto& tensor2data : tensor2data_list) {
              auto it = tensor2data.find(output->id());
              HT_ASSERT (it != tensor2data.end()) << "Something wrong! Can't find the data to fetch.";
              result.push_back(tensor2data[output->id()]);
            }
            results[it->second] = NDArray::cat(result);
          }
        }
        to_sync_op_ids.insert(op->id());
      }
    });
  }
  // OpList sync_ops;
  for (auto op_id : to_sync_op_ids) {
    _op_indexing[op_id]->Sync(num_micro_batches - 1);
    // sync_ops.push_back(_op_indexing[op_id]);
  }
  // HT_LOG_DEBUG << local_device << ": sync ops = " << sync_ops;
  for (size_t i = 0; i < results.size(); i++)
    HT_LOG_TRACE << "results[" << i << "]: " << results[i];
  HT_LOG_DEBUG << local_device << ": 4. get results[end]";

  // ********************** Run Level Check Point **********************
  // 一次完整的optimizer update发生了
  // transfer param buffer如果存在需要被清理掉
  // origin param buffer不能被清理掉
  // accumulate grad buffer如果存在需要被清理掉
  // current grad buffer需要被清理掉
  // 2024.3.3 update
  // 考虑到单策略alloc和free具有一定耗时
  // 因此把transfer param buffer和current grad buffer的清理放在需要热切换的时候
  if (_run_level == RunLevel::UPDATE) {
    if (_accumulate_grad_buffer->IsAllocated()) {
      // 已经对fetches sync过了
      // 这里直接free即可
      _accumulate_grad_buffer->Free();
    }
    if (_use_current_grad_buffer) {
      HT_ASSERT(_current_grad_buffer->IsAllocated())
        << "current grad buffer should be allocated in RunLevel::UPDATE";
      // _current_grad_buffer->Free();
    }
    if (_memory_profile_level == MEMORY_PROFILE_LEVEL::INFO)
      GetCUDAProfiler(local_device)->PrintCurrMemoryInfo(name() + " run UPDATE end");
  }
  // ********************** Run Level Check Point **********************

  bool is_analysis_perf = false;
  if (is_analysis_perf || _straggler_flag) {
    if (_used_ranks.size() >= 2) {
      auto& mpi_comm_group = hetu::impl::comm::MPICommunicationGroup::GetOrCreate(_used_ranks);
      mpi_comm_group->Barrier(true);
    }
  }
  TOK(run);
  HT_LOG_DEBUG << local_device << ": total run time = " << COST_MSEC(run)
               << " ms";
  
  // get all micro batches memory consumption
  if (_memory_profile_level == MEMORY_PROFILE_LEVEL::MICRO_BATCH && _memory_log_file_path != "") {
    std::ofstream file;
    std::string suffix = "_" + std::to_string(hetu::impl::comm::GetWorldRank()) + ".txt";
    file.open(_memory_log_file_path + suffix, std::ios_base::app);
    if (file.is_open()) {
      file << "[" << std::endl;
    } else {
      HT_RUNTIME_ERROR << "Error opening the file";
    }
    auto size = _all_micro_batches_memory_info.size();
    for (size_t i = 0; i < size; i++) {
      if (i != size - 1) {
        file << *_all_micro_batches_memory_info[i] << "," << std::endl;
      } else {
        file << *_all_micro_batches_memory_info[i] << std::endl;
      }
    }
    file << "]";
    file.close();
  }

  // get op execute time, sort and analysis
  if (is_analysis_perf || _straggler_flag) {
    TIK(free);
    runtime_ctx_list.clear();
    tensor2data_list.clear();
    grad_accumulation.clear();
    TOK(free);
    HT_LOG_DEBUG << local_device
                 << ": free temporary memory time = " << COST_MSEC(free)
                 << " ms";

    std::vector<std::pair<int64_t, int64_t>> op_execute_time;
    for (auto& op_ref : _execute_plan.local_topo) {
      auto& op = op_ref.get();
      if (is_placeholder_op(op) || is_variable_op(op)) {
        continue;
      }
      if (is_pipeline_stage_send_op(op) || is_pipeline_stage_recv_op(op)) {
        continue;
      }
      // get time cost for all micro batches
      int64_t time_cost = 0;
      for (int i = 0; i < num_micro_batches; i++) {
        time_cost += op->TimeCost(i);
      }
      op_execute_time.push_back({op->id(), time_cost});
    }
    // p2p events
    for (int i = 0; i < _p2p_events.size() / 2; i++) {
      auto& start = _p2p_events[2 * i];
      auto& end = _p2p_events[2 * i + 1];
      // record the time of p2p for each pipeline micro-batch
      op_execute_time.push_back({-(i+1), end->TimeSince(*start)});
    }
    std::sort(op_execute_time.begin(), op_execute_time.end(), [](
      std::pair<int64_t, int64_t>& op_t1, std::pair<int64_t, int64_t>& op_t2) {
        return op_t1.second > op_t2.second;
      });
    double compute_time = 0;
    double tp_p2p_time = 0;
    double pp_p2p_time = 0;
    double tp_collective_time = 0;
    double dp_grad_reduce_time = 0;
    double optimizer_time = 0;
    double blocking_time = 0;
    double other_time = 0;
    std::ostringstream out;
    out << "Op Execute Time: ";
    int print_num = 10000;
    for (auto& op_time : op_execute_time) {
      if (op_time.first >= 0) {
        auto op = _op_indexing[op_time.first];
        // print top 10 op
        if (print_num-- > 0) {
          out << std::endl << local_device << ": " << op << "(type = " << op->type() << "), " << "time = " << op_time.second * 1.0 / 1e6 << " ms";
          if (op->num_inputs() > 0) {
            out << "; input shapes = ";
            for (auto& input : op->inputs()) {
              out << input->shape() << ", ";
            }
          }
          out << "; inputs = " << op->inputs();
        }
        if (op->stream_index() == kComputingStream) {
          if (is_optimizer_update_op(op)) {
            optimizer_time += op_time.second * 1.0 / 1e6;
          } else {
            compute_time += op_time.second * 1.0 / 1e6;
          }
        } else if (op->stream_index() == kP2PStream) {
          tp_p2p_time += op_time.second * 1.0 / 1e6;
        } else if (op->stream_index() == kCollectiveStream) {
          if (is_optimizer_update_op(op->output(0)->consumer(0))) {
            /*
            HT_LOG_WARN << op << " is dp grad reduce op"
              << ", input ds is " << op->input(0)->cur_ds_union().ds_union_info()
              << ", output ds is " << op->output(0)->cur_ds_union().ds_union_info();
            */
            dp_grad_reduce_time += op_time.second * 1.0 / 1e6;
          } 
          // TODO: consider two continuous grad op
          else {
            /*
            HT_LOG_WARN << op << " is tp collective op"
              << ", input ds is " << op->input(0)->cur_ds_union().ds_union_info()
              << ", output ds is " << op->output(0)->cur_ds_union().ds_union_info();
            */
            tp_collective_time += op_time.second * 1.0 / 1e6;
          }
        } else if (op->stream_index() == kBlockingStream) {
          blocking_time += op_time.second * 1.0 / 1e6;
        } else {
          other_time += op_time.second * 1.0 / 1e6;
        }        
      } else {
        out << std::endl << local_device << ": batch p2p " << -op_time.first << " : " << op_time.second * 1.0 / 1e6 << " ms";
        pp_p2p_time += op_time.second * 1.0 / 1e6;
      }
    }
    if (is_analysis_perf) {
      HT_LOG_INFO << local_device << ": " 
                  << "\ntotal run time: " << COST_MSEC(run) << " ms, "
                  << "compute time: " << compute_time << " ms, "
                  << "tp p2p time: " << tp_p2p_time << " ms, "
                  << "tp collective time: " << tp_collective_time << " ms, "
                  << "dp grad reduce time: " << dp_grad_reduce_time << " ms, "
                  << "pp p2p time(include bubble): " << pp_p2p_time << " ms, "
                  << "optimizer time: " << optimizer_time << " ms, "
                  << "blocking time: " << blocking_time << " ms, "
                  << "other time: " << other_time << " ms" << std::endl
                  << out.str();
    }
    if (_straggler_flag) {
      HT_LOG_WARN << local_device << ": " 
                  << "\ntotal run time: " << COST_MSEC(run) << " ms, "
                  << "compute time: " << compute_time << " ms, "
                  << "tp p2p time: " << tp_p2p_time << " ms, "
                  << "tp collective time: " << tp_collective_time << " ms, "
                  << "dp grad reduce time: " << dp_grad_reduce_time << " ms, "
                  << "pp p2p time(include bubble): " << pp_p2p_time << " ms, "
                  << "optimizer time: " << optimizer_time << " ms, "
                  << "blocking time: " << blocking_time << " ms, "
                  << "other time: " << other_time << " ms" << std::endl;
      if (_straggler_log_file_path != "") {
        if (_straggler_flag == 1) {
          ofstream_sync file(_straggler_log_file_path, std::ios_base::app);
          if (file.is_open()) {
            file << compute_time << std::endl;
          } else {
            HT_RUNTIME_ERROR << "Error opening the file";
          }
        } else if (_straggler_flag == 2) {
          ofstream_sync file(_straggler_log_file_path + "_" + std::to_string(hetu::impl::comm::GetWorldRank()) + ".txt", std::ios_base::app);
          if (file.is_open()) {
            file << "total run time: " << COST_MSEC(run) << " ms" << std::endl;
            file << "compute time: " << compute_time << " ms" << std::endl;
          } else {
            HT_RUNTIME_ERROR << "Error opening the file";
          }
        }
      }
    }
  }
  _p2p_events.clear();
  return results;
}

// TODO: merge two `Run` func
NDArrayList ExecutableGraph::Run(const TensorList& fetches,
                                 const FeedDict& feed_dict) {
  HT_RUNTIME_ERROR << "NotImplementedError";
  
  // TODO: For each pair of `fetches` and `feed_dict`,
  // deduce the optimal execution plan, and cache it.
  for (auto& fetch : fetches) {
    if (fetch->placement().is_undetermined()) {
      Instantiate(fetches, kCUDA);
      break;
    }
  }

  auto is_op_computed = [&](const Operator& op) -> bool {
    return Operator::all_output_tensors_of(op, [&](const Tensor& tensor) {
      return feed_dict.find(tensor->id()) != feed_dict.end();
    });
  };
  OpRefList topo = Graph::TopoSort(fetches, num_ops(), is_op_computed);

  RuntimeContext runtime_ctx(topo.size());
  Tensor2NDArrayMap tensor2data;
  tensor2data.reserve(topo.size());
  tensor2data.insert(feed_dict.begin(), feed_dict.end());
  NDArrayList results(fetches.size());
  std::unordered_map<TensorId, size_t> fetch_indices;
  for (size_t i = 0; i < fetches.size(); i++)
    fetch_indices[fetches.at(i)->id()] = i;
  std::unordered_set<OpId> to_sync_op_ids;
  to_sync_op_ids.reserve(fetches.size());

  for (auto& op_ref : topo) {
    auto& op = op_ref.get();
    // Question: Is it possible that some outputs are fed in
    // while the rest are not?
    bool computed = Operator::all_output_tensors_of(op, [&](Tensor& tensor) {
      return feed_dict.find(tensor->id()) != feed_dict.end();
    });
    if (computed)
      continue;

    NDArrayList inputs;
    inputs.reserve(op->num_inputs());
    for (size_t i = 0; i < op->num_inputs(); i++) {
      // TODO: Support async transfer. And this could be checked for once.
      auto& data = tensor2data[op->input(i)->id()];
      if (data->device() != op->input(i)->placement() ||
          data->dtype() != op->input(i)->dtype()) {
        tensor2data[op->input(i)->id()] =
          NDArray::to(data, op->input(i)->placement(), op->input(i)->dtype(),
                      op->stream_index());
      }
      inputs.push_back(tensor2data[op->input(i)->id()]);
    }
    auto outputs = op->Compute(inputs, runtime_ctx);

    for (size_t i = 0; i < outputs.size(); i++) {
      tensor2data.insert({op->output(i)->id(), outputs[i]});
      auto it = fetch_indices.find(op->output(i)->id());
      if (it != fetch_indices.end()) {
        results[it->second] = outputs[i];
        to_sync_op_ids.insert(op->id());
      }
    }
    // TODO: remove inputs that are no longer used
  }
  for (auto op_id : to_sync_op_ids) {
    _op_indexing[op_id]->Sync();
  }
  return results;
}

} // namespace graph
} // namespace hetu
