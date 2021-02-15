// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/base/Thread.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Exception.h"
#include "muduo/base/Logging.h"

#include <type_traits>

#include <errno.h>
#include <linux/unistd.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace muduo {
namespace detail {
/* 获取线程id */
pid_t gettid() {
  /* 获得/系统内唯一的线程pid */
  return static_cast<pid_t>(::syscall(SYS_gettid));
}
/* 在fork后该作什么 */
void afterFork() {
  /* 可能是在fork后子进程需要去做的 */
  /* 在fork后的线程号清空,线程名->main */
  muduo::CurrentThread::t_cachedTid = 0;
  muduo::CurrentThread::t_threadName = "main";
  CurrentThread::tid(); /* 重新获得线程号 */
  // no need to call pthread_atfork(NULL, NULL, &afterFork);
}

/*
注意到这个初始化类
初始化线程名，线程id，
并设置了atfork子进程的处理
*/
class ThreadNameInitializer {
public:
  ThreadNameInitializer() {
    muduo::CurrentThread::t_threadName = "main";
    CurrentThread::tid(); /* 刷新tid */
    pthread_atfork(NULL, NULL, &afterFork);
  }
};

/*
这个就是进程创建的时候生成的对象
一个ThreadNameInitializer全局的变量，
也就是说进程创建的时候就会执行
ThreadNameInitializer的构造函数
设置name,线程号，和atfork函数
*/
ThreadNameInitializer init;

/* 线程数据类 含有我们需要执行的函数,参数
  我们之后会将这个对象传递给pthread的参数，之后
  Thread::start中 startThread函数会去使用ThreadData
  执行相应的函数。
*/
struct ThreadData {
  typedef muduo::Thread::ThreadFunc ThreadFunc;
  ThreadFunc func_; /* 线程函数 */

  string name_;           /* 线程名 */
  pid_t *tid_;            /* 线程号 */
  CountDownLatch *latch_; /* 倒计时的指针.... */

  ThreadData(ThreadFunc func, const string &name, pid_t *tid,
             CountDownLatch *latch)
      : func_(std::move(func)), name_(name), tid_(tid), latch_(latch) {}

  void runInThread() {
    *tid_ = muduo::CurrentThread::tid(); /* 获得当前线程号 */
    tid_ = NULL;
    latch_->countDown(); /* 通知启动线程可以继续了 */
    latch_ = NULL;
    /* tid_,latch_变量不再使用因为它们都是传进来的栈变量，
    清空比较好，防止后面误使用 */
    muduo ::CurrentThread::t_threadName =
        name_.empty() ? "muduoThread" : name_.c_str();
    ::prctl(PR_SET_NAME,
            muduo::CurrentThread::t_threadName); /* 设置本线程的名称 */
    try {
      func_(); /* 执行任务函数 */
      muduo::CurrentThread::t_threadName = "finished";
      /* 异常处理 */
    } catch (const Exception &ex) {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "exception caught in Thread %s\n", name_.c_str());
      fprintf(stderr, "reason: %s\n", ex.what());
      fprintf(stderr, "stack trace: %s\n", ex.stackTrace());
      abort();
    } catch (const std::exception &ex) {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "exception caught in Thread %s\n", name_.c_str());
      fprintf(stderr, "reason: %s\n", ex.what());
      abort();
    } catch (...) {
      muduo::CurrentThread::t_threadName = "crashed";
      fprintf(stderr, "unknown exception caught in Thread %s\n", name_.c_str());
      throw; // rethrow
    }
  }
};

void *startThread(void *obj) {
  ThreadData *data = static_cast<ThreadData *>(obj);
  data->runInThread();
  delete data;
  return NULL;
}

} // namespace detail

/* 缓存本线程线程号 */
void CurrentThread::cacheTid() {
  /* 如果当前线程号是0,
    那么调用gettid获取当前线程号 */
  if (t_cachedTid == 0) {
    t_cachedTid = detail::gettid();
    t_tidStringLength = snprintf(t_tidString, sizeof t_tidString, "%5d ",
                                 t_cachedTid); /* id的字符串形式 */
  }
}

/* 判断本线程是否是主线程(和进程号对比) */
bool CurrentThread::isMainThread() { return tid() == ::getpid(); }

/* 睡眠多久微秒 */
void CurrentThread::sleepUsec(int64_t usec) {
  struct timespec ts = {0, 0};
  ts.tv_sec = static_cast<time_t>(usec / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec =
      static_cast<long>(usec % Timestamp::kMicroSecondsPerSecond * 1000);
  ::nanosleep(&ts, NULL);
}

AtomicInt32 Thread::numCreated_;
/*
  线程对象创建并不直接跑，而是设置线程的名字为Threadx
  而是等到Thread.start才开始跑
  latch_倒计时器设置为1,
  意味着只有主线程会等待线程启动完毕
 */
Thread::Thread(ThreadFunc func, const string &n)
    : started_(false), joined_(false), pthreadId_(0), tid_(0),
      func_(std::move(func)), name_(n), latch_(1) {
  setDefaultName();
}
/*
  线程的析构中在线程如何还活着的情况
  使用了detach，意味着pthreadId_线程还在跑呢，
  意味着不用本线程回收pthreadId_线程的资源。
  所以分离了线程，保证pthreadId_线程依旧正常的运作
*/
Thread::~Thread() {
  if (started_ && !joined_) {
    pthread_detach(pthreadId_);
  }
}

/* 设置线程的名字 */
void Thread::setDefaultName() {
  int num = numCreated_.incrementAndGet();
  if (name_.empty()) {
    char buf[32];
    snprintf(buf, sizeof buf, "Thread%d", num);
    name_ = buf;
  }
}

/* 启动线程，ThreadData
  线程数据中的func_,name,tid,latch
*/
void Thread::start() {
  assert(!started_);
  started_ = true;
  // FIXME: move(func_)
  detail::ThreadData *data =
      new detail::ThreadData(func_, name_, &tid_, &latch_);
  if (pthread_create(&pthreadId_, NULL, &detail::startThread, data)) {
    started_ = false;
    delete data; // or no delete?
    LOG_SYSFATAL << "Failed in pthread_create";
  } else {
    latch_.wait();/* 等待新线程内部执行完tid的获取 */
    assert(tid_ > 0);
  }
}
/* 注意到这个线程应该是
其他线程对这个Thread对象
去执行join 资源回收 */
int Thread::join() {
  assert(started_);                      /* 断言已启动 */
  assert(!joined_);                      /* 断言尚未执行join */
  joined_ = true;                        /* 设置了该线程joined的标志 */
  return pthread_join(pthreadId_, NULL); /*  执行join */
}

} // namespace muduo
