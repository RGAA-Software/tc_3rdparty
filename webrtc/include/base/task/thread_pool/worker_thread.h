// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TASK_THREAD_POOL_WORKER_THREAD_H_
#define BASE_TASK_THREAD_POOL_WORKER_THREAD_H_

#include "base/base_export.h"
#include "base/compiler_specific.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/common/checked_lock.h"
#include "base/task/thread_pool/task_source.h"
#include "base/task/thread_pool/task_tracker.h"
#include "base/task/thread_pool/tracked_ref.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"

namespace base {

class WorkerThreadObserver;

namespace internal {

class TaskTracker;

// A worker that manages a single thread to run Tasks from TaskSources returned
// by a delegate.
//
// A WorkerThread starts out sleeping. It is woken up by a call to WakeUp().
// After a wake-up, a WorkerThread runs Tasks from TaskSources returned by
// the GetWork() method of its delegate as long as it doesn't return nullptr. It
// also periodically checks with its TaskTracker whether shutdown has completed
// and exits when it has.
//
// This class is thread-safe.
class BASE_EXPORT WorkerThread : public RefCountedThreadSafe<WorkerThread>,
                                 public PlatformThread::Delegate {
 public:
  // Labels this WorkerThread's association. This doesn't affect any logic
  // but will add a stack frame labeling this thread for ease of stack trace
  // identification
  enum class ThreadLabel {
    POOLED,
    SHARED,
    DEDICATED,
#if BUILDFLAG(IS_WIN)
    SHARED_COM,
    DEDICATED_COM,
#endif  // BUILDFLAG(IS_WIN)
  };

  // Delegate interface for WorkerThread. All methods are called from the
  // thread managed by the WorkerThread instance.
  class BASE_EXPORT Delegate {
   public:
    virtual ~Delegate() = default;

    // Returns the ThreadLabel the Delegate wants its WorkerThreads' stacks
    // to be labeled with.
    virtual ThreadLabel GetThreadLabel() const;

    // Called by |worker|'s thread when it enters its main function.
    virtual void OnMainEntry(WorkerThread* worker) = 0;

    // Called by |worker|'s thread to get a TaskSource from which to run a Task.
    virtual RegisteredTaskSource GetWork(WorkerThread* worker) = 0;

    // Called by the worker thread to swap the task source that has just run for
    // another one, if available. |task_source| must not be null. The worker can
    // then run the task returned as if it was acquired via GetWork().
    virtual RegisteredTaskSource SwapProcessedTask(
        RegisteredTaskSource task_source,
        WorkerThread* worker) = 0;

    // Called to determine how long to sleep before the next call to GetWork().
    // GetWork() may be called before this timeout expires if the worker's
    // WakeUp() method is called.
    virtual TimeDelta GetSleepTimeout() = 0;

    // Called by the WorkerThread's thread to wait for work.
    virtual void WaitForWork();

    // Called by |worker|'s thread right before the main function exits. The
    // Delegate is free to release any associated resources in this call. It is
    // guaranteed that WorkerThread won't access the Delegate or the
    // TaskTracker after calling OnMainExit() on the Delegate.
    virtual void OnMainExit(WorkerThread* worker) {}

    // Called by a WorkerThread when it is woken up without any work being
    // available for it to run.
    virtual void RecordUnnecessaryWakeup() {}

    static constexpr TimeDelta kPurgeThreadCacheIdleDelay = Seconds(1);

   protected:
    friend WorkerThread;
    static bool IsDelayFirstWorkerSleepEnabled();

    // Called in WaitForWork() to hide the worker's synchronization
    // mechanism. Returns |true| if signaled, and |false| if the call timed out.
    virtual bool TimedWait(TimeDelta timeout) = 0;

#if BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC) && \
    PA_CONFIG(THREAD_CACHE_SUPPORTED)
    // Returns the desired sleep time before the worker has to wake up to purge
    // the cache thread or reclaim itself. |min_sleep_time| contains the minimal
    // acceptable amount of time to sleep.
    static TimeDelta GetSleepTimeBeforePurge(TimeDelta min_sleep_time);
#endif  // BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC) &&
        // PA_CONFIG(THREAD_CACHE_SUPPORTED)
  };

  // Creates a WorkerThread that runs Tasks from TaskSources returned by
  // |delegate()|. No actual thread will be created for this WorkerThread before
  // Start() is called. |thread_type_hint| is the preferred thread type; the
  // actual thread type depends on shutdown state and platform
  // capabilities. |task_tracker| is used to handle shutdown behavior of
  // Tasks. |sequence_num| is an index that helps identifying this
  // WorkerThread. |predecessor_lock| is a lock that is allowed to be held when
  // calling methods on this WorkerThread.  Either JoinForTesting() or Cleanup()
  // must be called before releasing the last external reference.
  WorkerThread(ThreadType thread_type_hint,
               TrackedRef<TaskTracker> task_tracker,
               size_t sequence_num,
               const CheckedLock* predecessor_lock = nullptr);

  WorkerThread(const WorkerThread&) = delete;
  WorkerThread& operator=(const WorkerThread&) = delete;

  // Creates a thread to back the WorkerThread. The thread will be in a wait
  // state pending a WakeUp() call. No thread will be created if Cleanup() was
  // called. `io_thread_task_runner` is used to setup FileDescriptorWatcher on
  // worker threads. `io_thread_task_runner` must refer to a Thread with
  // MessgaePumpType::IO. If specified, |worker_thread_observer| will be
  // notified when the worker enters and exits its main function. It must not be
  // destroyed before JoinForTesting() has returned (must never be destroyed in
  // production). Returns true on success.
  bool Start(scoped_refptr<SingleThreadTaskRunner> io_thread_task_runner_,
             WorkerThreadObserver* worker_thread_observer = nullptr);


  // Returns true if the worker is alive.
  bool ThreadAliveForTesting() const;

  // Makes a request to cleanup the worker. This may be called from any thread.
  // The caller is expected to release its reference to this object after
  // calling Cleanup(). Further method calls after Cleanup() returns are
  // undefined.
  //
  // Expected Usage:
  //   scoped_refptr<WorkerThread> worker_ = /* Existing Worker */
  //   worker_->Cleanup();
  //   worker_ = nullptr;
  virtual void Cleanup() = 0;

  virtual Delegate* delegate() = 0;

  // Possibly updates the thread type to the appropriate type based on the
  // thread type hint, current shutdown state, and platform capabilities.
  // Must be called on the thread managed by |this|.
  void MaybeUpdateThreadType();

  // Informs this WorkerThread about periods during which it is not being
  // used. Thread-safe.
  void BeginUnusedPeriod();
  void EndUnusedPeriod();
  // Returns the last time this WorkerThread was used. Returns a null time if
  // this WorkerThread is currently in-use. Thread-safe.
  TimeTicks GetLastUsedTime() const;

  size_t sequence_num() const { return sequence_num_; }

 protected:
  friend class RefCountedThreadSafe<WorkerThread>;
  class Thread;

  ~WorkerThread() override;

  // Must be called by implementations on destruction.
  void Destroy();

  bool ShouldExit() const;

  // Returns the thread type to use based on the thread type hint, current
  // shutdown state, and platform capabilities.
  ThreadType GetDesiredThreadType() const;

  // Changes the thread type to |desired_thread_type|. Must be called on the
  // thread managed by |this|.
  void UpdateThreadType(ThreadType desired_thread_type);

  // PlatformThread::Delegate:
  void ThreadMain() override;

  // Dummy frames to act as "RunLabeledWorker()" (see RunMain() below). Their
  // impl is aliased to prevent compiler/linker from optimizing them out.
  void RunPooledWorker();
  void RunBackgroundPooledWorker();
  void RunSharedWorker();
  void RunBackgroundSharedWorker();
  void RunDedicatedWorker();
  void RunBackgroundDedicatedWorker();
#if BUILDFLAG(IS_WIN)
  void RunSharedCOMWorker();
  void RunBackgroundSharedCOMWorker();
  void RunDedicatedCOMWorker();
  void RunBackgroundDedicatedCOMWorker();
#endif  // BUILDFLAG(IS_WIN)

  // The real main, invoked through :
  //     ThreadMain() -> RunLabeledWorker() -> RunWorker().
  // "RunLabeledWorker()" is a dummy frame based on ThreadLabel+ThreadType
  // and used to easily identify threads in stack traces.
  NOT_TAIL_CALLED void RunWorker();

  // Self-reference to prevent destruction of |this| while the thread is alive.
  // Set in Start() before creating the thread. Reset in ThreadMain() before the
  // thread exits. No lock required because the first access occurs before the
  // thread is created and the second access occurs on the thread.
  scoped_refptr<WorkerThread> self_;

  mutable CheckedLock thread_lock_;

  // Handle for the thread managed by |this|.
  PlatformThreadHandle thread_handle_ GUARDED_BY(thread_lock_);

  // The last time this worker was used by its owner (e.g. to process work or
  // stand as a required idle thread).
  TimeTicks last_used_time_ GUARDED_BY(thread_lock_);

  // Whether the thread should exit. Set by Cleanup().
  AtomicFlag should_exit_;

  const TrackedRef<TaskTracker> task_tracker_;

  // Optional observer notified when a worker enters and exits its main
  // function. Set in Start() and never modified afterwards.
  raw_ptr<WorkerThreadObserver> worker_thread_observer_ = nullptr;

  // Desired thread type.
  const ThreadType thread_type_hint_;

  // Actual thread type. Can be different than |thread_type_hint_|
  // depending on system capabilities and shutdown state. No lock required
  // because all post-construction accesses occur on the thread.
  ThreadType current_thread_type_;

  // Whether this worker has been joined for testing.
  virtual bool join_called_for_testing() const = 0;

  const size_t sequence_num_;

  // Service thread task runner.
  scoped_refptr<SingleThreadTaskRunner> io_thread_task_runner_;
};

}  // namespace internal
}  // namespace base

#endif  // BASE_TASK_THREAD_POOL_WORKER_THREAD_H_
