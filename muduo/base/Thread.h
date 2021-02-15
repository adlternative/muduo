// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_THREAD_H
#define MUDUO_BASE_THREAD_H

#include "muduo/base/Atomic.h"
#include "muduo/base/CountDownLatch.h"
#include "muduo/base/Types.h"

#include <functional>
#include <memory>
#include <pthread.h>

namespace muduo {

class Thread : noncopyable {
public:
  typedef std::function<void()> ThreadFunc;

  explicit Thread(ThreadFunc, const string &name = string());
  // FIXME: make it movable in C++11
  ~Thread();

  void start(); /* 线程启动 */
  int join();   // return pthread_join()

  bool started() const { return started_; } /* 线程是否已经启动 */
  // pthread_t pthreadId() const { return pthreadId_; }
  pid_t tid() const { return tid_; }           /* 返回线程号 */
  const string &name() const { return name_; } /* 返回线程名 */

  static int numCreated() {
    return numCreated_.get();
  } /* 有多少个线程创建了? */

private:
  void setDefaultName(); /* 注意到这个函数私有 */

  bool started_;         /* 是否启动了 */
  bool joined_;          /* 是否被join */
  pthread_t pthreadId_;  /* 可能有问题的线程号 */
  pid_t tid_;            /* 真*线程号 */
  ThreadFunc func_;      /* 函数 */
  string name_;          /* 名称 */
  CountDownLatch latch_; /* 计数器 */

  static AtomicInt32 numCreated_; /* 静态原子变量 */
};

} // namespace muduo
#endif // MUDUO_BASE_THREAD_H
