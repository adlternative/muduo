// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_CURRENTTHREAD_H
#define MUDUO_BASE_CURRENTTHREAD_H

#include "muduo/base/Types.h"

namespace muduo {
namespace CurrentThread {
/* __thread 线程私有 */
// internal
extern __thread int t_cachedTid;/* 缓存的线程号 */
extern __thread char t_tidString[32];/*  */
extern __thread int t_tidStringLength;/*  */
extern __thread const char *t_threadName;/* 线程名 */
void cacheTid();

inline int tid() {
  /* likely(t_cachedTid==0为假)返回(t_cachedTid == 0) */
  if (__builtin_expect(t_cachedTid == 0, 0)) {
    /*也就意味着少概率情况下调用catcheTid获得当前线程的tid.  */
    cacheTid();
  }
  return t_cachedTid;
}

inline const char *tidString() // for logging
{
  return t_tidString;
}

inline int tidStringLength() // for logging
{
  return t_tidStringLength;
}

inline const char *name() { return t_threadName; }

bool isMainThread();

void sleepUsec(int64_t usec); // for testing

string stackTrace(bool demangle);
} // namespace CurrentThread
} // namespace muduo

#endif // MUDUO_BASE_CURRENTTHREAD_H
