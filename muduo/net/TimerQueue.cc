// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
/* OK */
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include "muduo/net/TimerQueue.h"

#include "muduo/base/Logging.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/Timer.h"
#include "muduo/net/TimerId.h"

#include <sys/timerfd.h>
#include <unistd.h>

namespace muduo
{
namespace net
{
namespace detail
{
/* 创建timefd */
int createTimerfd()
{
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                 TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0)
  {
    LOG_SYSFATAL << "Failed in timerfd_create";
  }
  return timerfd;
}
/* Timestamp -> timespec （s and ns） */
struct timespec howMuchTimeFromNow(Timestamp when)
{
  int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();
  if (microseconds < 100)
  {
    microseconds = 100;
  }
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(
      microseconds / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}
/* 读timer_fd log记录一下当前时间 */
void readTimerfd(int timerfd, Timestamp now)
{
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at " << now.toString();
  if (n != sizeof howmany)
  {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
  }
}

/* 重置超时时间 */
void resetTimerfd(int timerfd, Timestamp expiration)
{
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;
  struct itimerspec oldValue;
  memZero(&newValue, sizeof newValue);
  memZero(&oldValue, sizeof oldValue);
  /* 通过算出过期时间到现在的时间差（timespec） */
  newValue.it_value = howMuchTimeFromNow(expiration);
  /* 以timespec设置新的闹钟时间 */
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
  if (ret)
  {
    LOG_SYSERR << "timerfd_settime()";
  }
}

}  // namespace detail
}  // namespace net
}  // namespace muduo

using namespace muduo;
using namespace muduo::net;
using namespace muduo::net::detail;

TimerQueue::TimerQueue(EventLoop* loop)
  : loop_(loop),
    timerfd_(createTimerfd()),/* 创建timefd */
    timerfdChannel_(loop, timerfd_),/* 创建timefd通道 */
    timers_(),
    callingExpiredTimers_(false)
{
  /* timefd通道设置读回调并注册可读 */
  timerfdChannel_.setReadCallback(
      std::bind(&TimerQueue::handleRead, this));
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue()
{
  timerfdChannel_.disableAll();
  timerfdChannel_.remove();
  ::close(timerfd_);
  // do not remove channel, since we're in EventLoop::dtor();
  for (const Entry& timer : timers_)
  {
    delete timer.second;
  }
}

/* 添加某个定时任务和时间 */
TimerId TimerQueue::addTimer(TimerCallback cb,
                             Timestamp when,
                             double interval)
{
  /* 生成一个新的Timer对象 */
  Timer* timer = new Timer(std::move(cb), when, interval);
  /*在IO线程中调用addTimerInLoop，*/
  loop_->runInLoop(
      std::bind(&TimerQueue::addTimerInLoop, this, timer));
  return TimerId(timer, timer->sequence());
}

/* 取消某个定时器 */
void TimerQueue::cancel(TimerId timerId)
{
  /* 在iO线程调cancelInLoop */
  loop_->runInLoop(
      std::bind(&TimerQueue::cancelInLoop, this, timerId));
}

/* 添加到定时器集合中，并可能的修改timefd唤醒时间 */
void TimerQueue::addTimerInLoop(Timer* timer)
{
  loop_->assertInLoopThread();
  bool earliestChanged = insert(timer);

  if (earliestChanged)
  {
    /* 因为我们是通过一个timefd进行通讯的，
    所以我们需要更新超时时间，这样才能及时的唤醒EPoll */
    resetTimerfd(timerfd_, timer->expiration());
  }
}

/* 在定时器集合中取消TimerId的对应Timer */
void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  ActiveTimer timer(timerId.timer_, timerId.sequence_);
  ActiveTimerSet::iterator it = activeTimers_.find(timer);
  if (it != activeTimers_.end())
  {
    /* 找到了则从timers_,activeTimers_中删除 */
    size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
    assert(n == 1); (void)n;
    delete it->first; // FIXME: no delete please
    activeTimers_.erase(it);
  }
  else if (callingExpiredTimers_)
  {
    /* 说明现在正在处理定时器任务,
    这时候我们不可以cannel直接删除定时器，
    而是会被加入到cannel定时器集合中 */
    cancelingTimers_.insert(timer);
  }
  assert(timers_.size() == activeTimers_.size());
}

/* timerfd唤醒Epoll后会做的事情 */
void TimerQueue::handleRead()
{
  loop_->assertInLoopThread();
  Timestamp now(Timestamp::now());
  /* 读取timerfd_ */
  readTimerfd(timerfd_, now);
  /* 获取过期的Entry向量 */
  std::vector<Entry> expired = getExpired(now);
  /* 这时候我们在处理超时任务， */
  callingExpiredTimers_ = true;
  cancelingTimers_.clear();
  // safe to callback outside critical section
  for (const Entry& it : expired)
  {
    it.second->run();
  }
  callingExpiredTimers_ = false;
  /* 一轮定时任务处理完成，重置 */
  reset(expired, now);
}

/* 获得过期的定时器集合 */
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  assert(timers_.size() == activeTimers_.size());
  std::vector<Entry> expired;
  Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));/* 当前时间 */
  TimerList::iterator end = timers_.lower_bound(sentry);/* 找到当前时间之前并过期时间最近的那个定时器 */
  assert(end == timers_.end() || now < end->first);
  std::copy(timers_.begin(), end, back_inserter(expired));/* 将之前的所有定时器都插入到过期的定时器向量中 */
  timers_.erase(timers_.begin(), end);/* 删除前面这些定时器 */

  for (const Entry& it : expired)
  {
    ActiveTimer timer(it.second, it.second->sequence());
    size_t n = activeTimers_.erase(timer);/* 删除对应的activeTimer */
    assert(n == 1); (void)n;
  }

  assert(timers_.size() == activeTimers_.size());
  return expired;
}

/* 在进行一轮定时任务之后，重置各个重复执行的定时器，并重置timefd唤醒时间 */
void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
  Timestamp nextExpire;

  for (const Entry& it : expired)
  {
    ActiveTimer timer(it.second, it.second->sequence());
    /* 定时任务重复执行并且没有被取消 */
    if (it.second->repeat()
        && cancelingTimers_.find(timer) == cancelingTimers_.end())
    {
      /* 重置定时器过期时间 */
      it.second->restart(now);
      /* 插入到相应集合中 */
      insert(it.second);
    }
    else
    {
      /* 否则删除 */
      // FIXME move to a free list
      delete it.second; // FIXME: no delete please
    }
  }

  if (!timers_.empty())
  {
    nextExpire = timers_.begin()->second->expiration();
  }
  /* 重置timerfd_唤醒时间 */
  if (nextExpire.valid())
  {
    resetTimerfd(timerfd_, nextExpire);
  }
}

/* 插入一个Timer 到定时器集合和活跃定时器集合中 */
bool TimerQueue::insert(Timer* timer)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  bool earliestChanged = false;
  Timestamp when = timer->expiration();/* 获得过期时间 */
  TimerList::iterator it = timers_.begin();
  if (it == timers_.end() || when < it->first)
  {
    earliestChanged = true;/* 最早过期的时间修改 */
  }
  {
    std::pair<TimerList::iterator, bool> result
      = timers_.insert(Entry(when, timer));/* {expiration,Timer*}插入到 timers集合中 */
    assert(result.second); (void)result;
  }
  {
    std::pair<ActiveTimerSet::iterator, bool> result
      = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));/* {Timer*,seq}插入到 活跃Timers 集合中 */
    assert(result.second); (void)result;
  }

  assert(timers_.size() == activeTimers_.size());
  return earliestChanged;
}

