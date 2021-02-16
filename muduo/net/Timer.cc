// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
/* OK */
#include "muduo/net/Timer.h"

using namespace muduo;
using namespace muduo::net;

AtomicInt64 Timer::s_numCreated_;

void Timer::restart(Timestamp now)
{
  if (repeat_)
  {
    /* 重复的话新时间就是当前时间加上间隔时间。 */
    expiration_ = addTime(now, interval_);
  }
  else
  {
    /* 不重复则不该restart, 则返回一个非法时间 */
    expiration_ = Timestamp::invalid();
  }
}
