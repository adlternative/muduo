// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/poller/EPollPoller.h"

#include "muduo/base/Logging.h"
#include "muduo/net/Channel.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

// On Linux, the constants of poll(2) and epoll(4)
// are expected to be the same.
static_assert(EPOLLIN == POLLIN,        "epoll uses same flag values as poll");
static_assert(EPOLLPRI == POLLPRI,      "epoll uses same flag values as poll");
static_assert(EPOLLOUT == POLLOUT,      "epoll uses same flag values as poll");
static_assert(EPOLLRDHUP == POLLRDHUP,  "epoll uses same flag values as poll");
static_assert(EPOLLERR == POLLERR,      "epoll uses same flag values as poll");
static_assert(EPOLLHUP == POLLHUP,      "epoll uses same flag values as poll");

namespace
{
const int kNew = -1;
const int kAdded = 1;
const int kDeleted = 2;
}

/*
绑定loop(基类的构造函数)
epoll_create1 获得 epoll_fd
*/
EPollPoller::EPollPoller(EventLoop* loop)
  : Poller(loop),
    epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
    events_(kInitEventListSize)
{
  if (epollfd_ < 0)
  {
    LOG_SYSFATAL << "EPollPoller::EPollPoller";
  }
}

/* 析构函数中关闭 epoll_fd */
EPollPoller::~EPollPoller()
{
  ::close(epollfd_);
}

/* epoll_wait 可以指定等待时间 */
Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
  LOG_TRACE << "fd total count " << channels_.size();
  /* 传入 ChannelList event_ 首个元素的地址 */
  int numEvents = ::epoll_wait(epollfd_,
                               &*events_.begin(),
                               static_cast<int>(events_.size()),
                               timeoutMs);
  int savedErrno = errno;
  /* 记录时间epoll_wait返回时间 */
  Timestamp now(Timestamp::now());
  if (numEvents > 0)
  {
    LOG_TRACE << numEvents << " events happened";
    fillActiveChannels(numEvents, activeChannels);
    /* 如果活跃的Channel 的数量和 ChannelList events_ 大小相同 ，events_扩容 */
    if (implicit_cast<size_t>(numEvents) == events_.size())
    {
      events_.resize(events_.size()*2);
    }
  }
  else if (numEvents == 0)
  {
    /* 感觉这不像是可能发生的情况？ */
    LOG_TRACE << "nothing happened";
  }
  else
  {
    // error happens, log uncommon ones
    if (savedErrno != EINTR)
    {
      errno = savedErrno;
      LOG_SYSERR << "EPollPoller::poll()";
    }
  }
  return now;
}

/* 添加到活跃通道列表中 */
void EPollPoller::fillActiveChannels(int numEvents,
                                     ChannelList* activeChannels) const
{
  assert(implicit_cast<size_t>(numEvents) <= events_.size());
  for (int i = 0; i < numEvents; ++i)
  {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
#ifndef NDEBUG
    int fd = channel->fd();
    ChannelMap::const_iterator it = channels_.find(fd);
    assert(it != channels_.end());
    assert(it->second == channel);
#endif
    /* 通道记录返回的事件类型 */
    channel->set_revents(events_[i].events);
    /* 活跃的通道中加入该通道 */
    activeChannels->push_back(channel);
  }
}

/* 在epoll中更新一个通道的注册事件 */
void EPollPoller::updateChannel(Channel* channel)
{
  Poller::assertInLoopThread();
  const int index = channel->index();
  LOG_TRACE << "fd = " << channel->fd()
    << " events = " << channel->events() << " index = " << index;
  /* 如果是“空”的通道或者是已经被删除的通道 */
  if (index == kNew || index == kDeleted)
  {
    // a new one, add with EPOLL_CTL_ADD
    int fd = channel->fd();
    /* kNew: 那么字典中是找不到的对应的{fd,channel}键值对的，
      那么我们将它添加到进入字典 */
    if (index == kNew)
    {
      assert(channels_.find(fd) == channels_.end());
      channels_[fd] = channel;
    }
    else // index == kDeleted
    {
      /* KDeleted: 字典中还存在该通道 */
      assert(channels_.find(fd) != channels_.end());
      assert(channels_[fd] == channel);
    }
    /*updateChannel之后的channel.index设置为kAdded  */
    channel->set_index(kAdded);
    /* 在epoll上执行add进行监听新的频道 */
    update(EPOLL_CTL_ADD, channel);
  }
  else // index == kAdded
  {
    // update existing one with EPOLL_CTL_MOD/DEL
    int fd = channel->fd();
    (void)fd;
    assert(channels_.find(fd) != channels_.end());
    assert(channels_[fd] == channel);
    assert(index == kAdded);
    /* 如果一个状态是kAdded的channel执行updateChannel*/
    /*若没有注册事件*/
    if (channel->isNoneEvent())
    {
      /* 那么就EPOLL_CTL_DEL
      去从epoll上取消
      对该channel的监听 */
      update(EPOLL_CTL_DEL, channel);
      /* 更新channel的状态为已从epoll删除 */
      channel->set_index(kDeleted);
    }
    /*若注册了事件*/
    else
    {
      /* 修改需要监听的事件 */
      update(EPOLL_CTL_MOD, channel);
    }
  }
}

/* 在epoll中删除一个通道 */
void EPollPoller::removeChannel(Channel* channel)
{
  /* 保证在IO线程 */
  Poller::assertInLoopThread();
  int fd = channel->fd();
  LOG_TRACE << "fd = " << fd;
  assert(channels_.find(fd) != channels_.end());
  assert(channels_[fd] == channel);
  assert(channel->isNoneEvent());
  int index = channel->index();
  assert(index == kAdded || index == kDeleted);
  /* 在字典中删除对应的键值对 */
  size_t n = channels_.erase(fd);
  (void)n;
  assert(n == 1);
  /* 如果是一个监听中的频道，
    我们取消监听 */
  if (index == kAdded)
  {
    update(EPOLL_CTL_DEL, channel);
  }
  /* 只有在remove的时候
    才会从原有的键值对从字典中删除,
    并将通道的index设置为kNew，而update中的
      update(EPOLL_CTL_DEL, channel);
    只是将通道从epoll中取消监听,
  然而channel指针仍然存在。 */
  channel->set_index(kNew);
}

/* epoll_ctl包装  */
void EPollPoller::update(int operation, Channel* channel)
{
  struct epoll_event event;
  memZero(&event, sizeof event);
  event.events = channel->events();
  event.data.ptr = channel;
  int fd = channel->fd();
  LOG_TRACE << "epoll_ctl op = " << operationToString(operation)
    << " fd = " << fd << " event = { " << channel->eventsToString() << " }";
  if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
  {
    if (operation == EPOLL_CTL_DEL)
    {
      LOG_SYSERR << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
    else
    {
      LOG_SYSFATAL << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
  }
}

/* epoll_ctl op ->string  */
const char* EPollPoller::operationToString(int op)
{
  switch (op)
  {
    case EPOLL_CTL_ADD:
      return "ADD";
    case EPOLL_CTL_DEL:
      return "DEL";
    case EPOLL_CTL_MOD:
      return "MOD";
    default:
      assert(false && "ERROR op");
      return "Unknown Operation";
  }
}
