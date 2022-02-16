// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler-dispatcher/optimizing-compile-dispatcher.h"

#include "src/base/atomicops.h"
#include "src/codegen/compiler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/execution/isolate.h"
#include "src/execution/local-isolate.h"
#include "src/heap/local-heap.h"
#include "src/heap/parked-scope.h"
#include "src/init/v8.h"
#include "src/logging/counters.h"
#include "src/logging/log.h"
#include "src/logging/runtime-call-stats-scope.h"
#include "src/objects/objects-inl.h"
#include "src/tasks/cancelable-task.h"
#include "src/tracing/trace-event.h"

namespace v8 {
namespace internal {

namespace {

void DisposeCompilationJob(OptimizedCompilationJob* job,
                           bool restore_function_code) {
  if (restore_function_code) {
    Handle<JSFunction> function = job->compilation_info()->closure();
    function->set_code(function->shared().GetCode(), kReleaseStore);
    if (function->IsInOptimizationQueue()) {
      function->ClearOptimizationMarker();
    }
    if (job->compilation_info()->is_osr()) {
      function->shared().set_osr_is_in_optimization_queue(false);
    }
  }
  delete job;
}

}  // namespace

class OptimizingCompileDispatcher::CompileTask : public CancelableTask {
 public:
  explicit CompileTask(Isolate* isolate,
                       OptimizingCompileDispatcher* dispatcher)
      : CancelableTask(isolate),
        isolate_(isolate),
        worker_thread_runtime_call_stats_(
            isolate->counters()->worker_thread_runtime_call_stats()),
        dispatcher_(dispatcher) {
    ++dispatcher_->ref_count_;
  }

  CompileTask(const CompileTask&) = delete;
  CompileTask& operator=(const CompileTask&) = delete;

  ~CompileTask() override = default;

 private:
  // v8::Task overrides.
  void RunInternal() override {
    LocalIsolate local_isolate(isolate_, ThreadKind::kBackground);
    DCHECK(local_isolate.heap()->IsParked());

    {
      RCS_SCOPE(&local_isolate,
                RuntimeCallCounterId::kOptimizeBackgroundDispatcherJob);

      TimerEventScope<TimerEventRecompileConcurrent> timer(isolate_);
      TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
                   "V8.OptimizeBackground");

      if (dispatcher_->recompilation_delay_ != 0) {
        base::OS::Sleep(base::TimeDelta::FromMilliseconds(
            dispatcher_->recompilation_delay_));
      }

      dispatcher_->CompileNext(dispatcher_->NextInput(&local_isolate),
                               &local_isolate);
    }
    {
      base::MutexGuard lock_guard(&dispatcher_->ref_count_mutex_);
      if (--dispatcher_->ref_count_ == 0) {
        dispatcher_->ref_count_zero_.NotifyOne();
      }
    }
  }

  Isolate* isolate_;
  WorkerThreadRuntimeCallStats* worker_thread_runtime_call_stats_;
  OptimizingCompileDispatcher* dispatcher_;
};

OptimizingCompileDispatcher::~OptimizingCompileDispatcher() {
  DCHECK_EQ(0, ref_count_);
  DCHECK_EQ(0, input_queue_length_);
  DeleteArray(input_queue_);
}

OptimizedCompilationJob* OptimizingCompileDispatcher::NextInput(
    LocalIsolate* local_isolate) {
  base::MutexGuard access_input_queue_(&input_queue_mutex_);
  if (input_queue_length_ == 0) return nullptr;
  OptimizedCompilationJob* job = input_queue_[InputQueueIndex(0)];
  DCHECK_NOT_NULL(job);
  input_queue_shift_ = InputQueueIndex(1);
  input_queue_length_--;
  return job;
}

void OptimizingCompileDispatcher::CompileNext(OptimizedCompilationJob* job,
                                              LocalIsolate* local_isolate) {
  if (!job) return;

  // The function may have already been optimized by OSR.  Simply continue.
  CompilationJob::Status status =
      job->ExecuteJob(local_isolate->runtime_call_stats(), local_isolate);
  USE(status);  // Prevent an unused-variable error.

  {
    // The function may have already been optimized by OSR.  Simply continue.
    // Use a mutex to make sure that functions marked for install
    // are always also queued.
    base::MutexGuard access_output_queue_(&output_queue_mutex_);
    output_queue_.push(job);
  }

  if (finalize()) isolate_->stack_guard()->RequestInstallCode();
}

void OptimizingCompileDispatcher::FlushOutputQueue(bool restore_function_code) {
  for (;;) {
    OptimizedCompilationJob* job = nullptr;
    {
      base::MutexGuard access_output_queue_(&output_queue_mutex_);
      if (output_queue_.empty()) return;
      job = output_queue_.front();
      output_queue_.pop();
    }

    DisposeCompilationJob(job, restore_function_code);
  }
}

void OptimizingCompileDispatcher::FlushInputQueue() {
  base::MutexGuard access_input_queue_(&input_queue_mutex_);
  while (input_queue_length_ > 0) {
    OptimizedCompilationJob* job = input_queue_[InputQueueIndex(0)];
    DCHECK_NOT_NULL(job);
    input_queue_shift_ = InputQueueIndex(1);
    input_queue_length_--;
    DisposeCompilationJob(job, true);
  }
}

void OptimizingCompileDispatcher::AwaitCompileTasks() {
  {
    base::MutexGuard lock_guard(&ref_count_mutex_);
    while (ref_count_ > 0) ref_count_zero_.Wait(&ref_count_mutex_);
  }

#ifdef DEBUG
  base::MutexGuard access_input_queue(&input_queue_mutex_);
  CHECK_EQ(input_queue_length_, 0);
#endif  // DEBUG
}

void OptimizingCompileDispatcher::FlushQueues(
    BlockingBehavior blocking_behavior, bool restore_function_code) {
  FlushInputQueue();
  if (blocking_behavior == BlockingBehavior::kBlock) {
    base::MutexGuard lock_guard(&ref_count_mutex_);
    while (ref_count_ > 0) ref_count_zero_.Wait(&ref_count_mutex_);
  }
  FlushOutputQueue(restore_function_code);
}

void OptimizingCompileDispatcher::Flush(BlockingBehavior blocking_behavior) {
  HandleScope handle_scope(isolate_);
  FlushQueues(blocking_behavior, true);
  if (FLAG_trace_concurrent_recompilation) {
    PrintF("  ** Flushed concurrent recompilation queues. (mode: %s)\n",
           (blocking_behavior == BlockingBehavior::kBlock) ? "blocking"
                                                           : "non blocking");
  }
}

void OptimizingCompileDispatcher::Stop() {
  HandleScope handle_scope(isolate_);
  FlushQueues(BlockingBehavior::kBlock, false);
  // At this point the optimizing compiler thread's event loop has stopped.
  // There is no need for a mutex when reading input_queue_length_.
  DCHECK_EQ(input_queue_length_, 0);
}

void OptimizingCompileDispatcher::InstallOptimizedFunctions() {
  HandleScope handle_scope(isolate_);

  for (;;) {
    OptimizedCompilationJob* job = nullptr;
    {
      base::MutexGuard access_output_queue_(&output_queue_mutex_);
      if (output_queue_.empty()) return;
      job = output_queue_.front();
      output_queue_.pop();
    }
    OptimizedCompilationInfo* info = job->compilation_info();
    Handle<JSFunction> function(*info->closure(), isolate_);
    if (function->HasAvailableCodeKind(info->code_kind()) && !info->is_osr()) {
      if (FLAG_trace_concurrent_recompilation) {
        PrintF("  ** Aborting compilation for ");
        function->ShortPrint();
        PrintF(" as it has already been optimized.\n");
      }
      DisposeCompilationJob(job, false);
    } else {
      Compiler::FinalizeOptimizedCompilationJob(job, isolate_);
    }
  }
}

bool OptimizingCompileDispatcher::HasJobs() {
  DCHECK_EQ(ThreadId::Current(), isolate_->thread_id());
  // Note: This relies on {output_queue_} being mutated by a background thread
  // only when {ref_count_} is not zero. Also, {ref_count_} is never incremented
  // by a background thread.
  return ref_count_ != 0 || !output_queue_.empty();
}

void OptimizingCompileDispatcher::QueueForOptimization(
    OptimizedCompilationJob* job) {
  DCHECK(IsQueueAvailable());
  {
    // Add job to the back of the input queue.
    base::MutexGuard access_input_queue(&input_queue_mutex_);
    DCHECK_LT(input_queue_length_, input_queue_capacity_);
    input_queue_[InputQueueIndex(input_queue_length_)] = job;
    input_queue_length_++;
  }
  V8::GetCurrentPlatform()->CallOnWorkerThread(
      std::make_unique<CompileTask>(isolate_, this));
}

}  // namespace internal
}  // namespace v8
