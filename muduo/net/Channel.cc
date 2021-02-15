// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/base/Logging.h"
#include "muduo/net/Channel.h"
#include "muduo/net/EventLoop.h"

#include <sstream>

#include <poll.h>

using namespace muduo;
using namespace muduo::net;

const int Channel::kNoneEvent = 0;/* 无事件 */
const int Channel::kReadEvent = POLLIN | POLLPRI;/* 读事件 IN PRI */
const int Channel::kWriteEvent = POLLOUT;/* 写事件 OUT */

/* 构造函数中绑定EVENTLOOP 和 fd */
Channel::Channel(EventLoop* loop, int fd__)
  : loop_(loop),
    fd_(fd__),
    events_(0),
    revents_(0),
    index_(-1),
    logHup_(true),
    tied_(false),
    eventHandling_(false),
    addedToLoop_(false)
{
}

Channel::~Channel()
{
  assert(!eventHandling_);
  assert(!addedToLoop_);
  if (loop_->isInLoopThread())
  {
    assert(!loop_->hasChannel(this));
  }
}

void Channel::tie(const std::shared_ptr<void>& obj)
{
  tie_ = obj;
  tied_ = true;
}

void Channel::update()
{
  addedToLoop_ = true;
  loop_->updateChannel(this);
}

void Channel::remove()
{
  assert(isNoneEvent());
  addedToLoop_ = false;
  loop_->removeChannel(this);
}

/* 处理事件  */
void Channel::handleEvent(Timestamp receiveTime)
{
  std::shared_ptr<void> guard;
  if (tied_)
  {
    guard = tie_.lock();
    if (guard)
    {
      handleEventWithGuard(receiveTime);
    }
  }
  else
  {
    handleEventWithGuard(receiveTime);
  }
}

/* 处理事件 需要先加锁？ */
void Channel::handleEventWithGuard(Timestamp receiveTime)
{
  eventHandling_ = true;
  LOG_TRACE << reventsToString();
  if ((revents_ & POLLHUP) && !(revents_ & POLLIN))
  {
    /* POLLHUP: 设备已断开连接，
    或者管道或FIFO已被打开以进行写入的最后一个进程关闭。
    设置后，FIFO的挂起状态将一直持续到某个进程打开FIFO进行写入
    或关闭FIFO的所有只读文件描述符为止。
    此事件和POLLOUT是互斥的；如果发生挂断，流将永远不可写。
    但是，此事件与POLLIN，POLLRDNORM，POLLRDBAND或POLLPRI并不互斥。
    该标志仅在revents位掩码中有效。在事件成员中应将其忽略。 */

    /* POLLHUP 对端断开可写到日志中 */
    if (logHup_)
    {
      LOG_WARN << "fd = " << fd_ << " Channel::handle_event() POLLHUP";
    }
    if (closeCallback_) closeCallback_();
  }

  /* Invalid polling request 打印日志
  POLLNVAL如果我关闭文件描述符，然后尝试从关闭的fd中读取，将触发该程序。
 */
  if (revents_ & POLLNVAL)
  {
    LOG_WARN << "fd = " << fd_ << " Channel::handle_event() POLLNVAL";
  }
  /* 处理错误事件 */
  if (revents_ & (POLLERR | POLLNVAL))
  {
    if (errorCallback_) errorCallback_();
  }
  /* 处理读事件 */
  if (revents_ & (POLLIN | POLLPRI | POLLRDHUP))
  {
    if (readCallback_) readCallback_(receiveTime);
  }
  /* 处理写事件 */
  if (revents_ & POLLOUT)
  {
    if (writeCallback_) writeCallback_();
  }
  eventHandling_ = false;
}

/* 将返回事件格式化输出 */
string Channel::reventsToString() const
{
  return eventsToString(fd_, revents_);
}

/* 将注册事件格式化输出 */
string Channel::eventsToString() const
{
  return eventsToString(fd_, events_);
}

/* 将事件格式化输出 */
string Channel::eventsToString(int fd, int ev)
{
  std::ostringstream oss;
  oss << fd << ": ";
  if (ev & POLLIN)
    oss << "IN ";
  if (ev & POLLPRI)
    oss << "PRI ";
  if (ev & POLLOUT)
    oss << "OUT ";
  if (ev & POLLHUP)
    oss << "HUP ";
  if (ev & POLLRDHUP)
    oss << "RDHUP ";
  if (ev & POLLERR)
    oss << "ERR ";
  if (ev & POLLNVAL)
    oss << "NVAL ";

  return oss.str();
}
