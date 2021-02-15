// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/Poller.h"

#include "muduo/net/Channel.h"

using namespace muduo;
using namespace muduo::net;

/* 池和loop绑定 */
Poller::Poller(EventLoop* loop)
  : ownerLoop_(loop)
{
}

Poller::~Poller() = default;
/* 在池的{fd,Channel}字典中寻找是否含有指定的Channel */
bool Poller::hasChannel(Channel* channel) const
{
  assertInLoopThread();
  ChannelMap::const_iterator it = channels_.find(channel->fd());
  return it != channels_.end() && it->second == channel;
}

