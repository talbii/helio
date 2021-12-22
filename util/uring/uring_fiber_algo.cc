// Copyright 2021, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/uring/uring_fiber_algo.h"

#include <sys/poll.h>
#include <sys/timerfd.h>

#include "base/logging.h"
#include "util/uring/proactor.h"

// TODO: We should replace DVLOG macros with RAW_VLOG if we do glog sync integration.

namespace util {
namespace uring {
using namespace boost;
using namespace std;

UringFiberAlgo::UringFiberAlgo(Proactor* proactor) : FiberSchedAlgo(proactor) {
}

UringFiberAlgo::~UringFiberAlgo() {
}

// suspend_until halts the thread in case there are no active fibers to run on it.
// This function is called by dispatcher fiber.
void UringFiberAlgo::SuspendWithTimer(const time_point& abs_time) noexcept {
  using namespace chrono;
  DCHECK(time_point::max() != abs_time);

  auto cb = [this](Proactor::IoResult res, uint32_t /*flags*/, int64_t payload, Proactor*) {
    // If io_uring does not support timeout, then this callback will be called
    // earlier than needed and dispatch won't awake the sleeping fiber.
    // This will cause deadlock.
    DCHECK_NE(res, -EINVAL) << "This linux version does not support this operation";
    DVLOG(1) << "this_fiber::yield " << res << " " << payload;
    if (this->active_timer_ns_ == payload) {
      this->active_timer_ns_ = 0;
    }
    this_fiber::yield();
  };

  // TODO: if we got here, most likely our completion queues were empty so
  // it's unlikely that we will have full submit queue but this state may happen.
  // GetSubmitEntry may block which may cause a deadlock since our main loop is not
  // running (it's probably in suspend mode letting dispatcher fiber to run).
  // Therefore we must use here non blocking calls.
  // But what happens if SQ is full?
  // SQ is full we can not use IoUring to schedule awake event, our CQ queue is empty so
  // we have nothing to process. We might want to give up on this timer and just wait on CQ
  // since we know something might come up. On the other hand, imagine we send requests on sockets
  // but they all do not answer so SQ is eventually full, CQ is empty and our IO loop is overflown
  // and no entries could be processed.
  // We must reproduce this case: small SQ/CQ. Fill SQ/CQ with alarms that expire in a long time.
  // So at some point SQ-push returns EBUSY. Now we call this_fiber::sleep and we GetSubmitEntry
  // would block.
  Proactor* proactor = (Proactor*)proactor_;

  if (io_uring_sq_space_left(&proactor->ring_) == 0) {
    ++proactor->dispatch_suspend_timer_fail_;
    return;
  }

  const chrono::time_point<steady_clock, nanoseconds>& tp = abs_time;
  int64_t ns = time_point_cast<nanoseconds>(tp).time_since_epoch().count();

  // SuspendWithTimer can be called with either:
  //   1. abs_time == active_timer_ns_ when we suspended multiple times before the dedline
  //      was reached.
  //   2. abs_time < active_timer_ when a new timer was introduced and it expires earlier than
  //      the active one.
  // Fibers dispatcher will not suspend the thread with abs_time greater than active_timer_ns_
  // because it can suspend until the earliest timepoint among all active timers present.
  if (active_timer_ns_ == ns) {  // we already have an active timer
    return;
  }

  SubmitEntry se = proactor->GetSubmitEntry(std::move(cb), ns);
  constexpr uint64_t kNsFreq = 1000000000ULL;
  ts_.tv_sec = ns / kNsFreq;
  ts_.tv_nsec = ns - ts_.tv_sec * kNsFreq;

  active_timer_ns_ = ns;

  // We require at least 5.8 for io_uring and get rid of those conditions.
  // Please note that we can not pass var on stack because we exit from the function
  // before we submit to ring. That's why ts_ is a data member.
  se.PrepTimeout(&ts_, true);
}

}  // namespace uring
}  // namespace util