
#ifndef WINTER_LIBEV
#define WINTER_LIBEV 1

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <memory.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <vector>
#include <map>
#include <string>

enum { kHighPriority = 4, kLowPriority = 1 };

enum {
  kNone = 0x00,
  kReadEvent = (0x01 << 0),
  kWriteEvent = (0x01 << 1),
  kTimeoutEvent = (0x01 << 2),
  kSignalEvent = (0x01 << 3),
  kBeforeLoop = (0x01 << 4),
  kAfterLoop = (0x01 << 5),
  kChildEvent = (0x01 << 6),
  kStatEvent = (0x01 << 7),
  kSocketCloseEvent = (0x01 << 8),
  kAsyncEvent = (0x01 << 9)
};

struct Event;
struct EventNode;
struct EventLoop;
typedef void (*EventProc)(EventLoop *loop, Event *event, int revent);

struct Event {
  Event()
      : m_active(0), m_pending(0), m_priority(kLowPriority), m_data(NULL),
        m_proc(NULL) {}

  int m_active;
  int m_pending;
  int m_priority;
  void *m_data;
  EventProc m_proc;
};

struct EventNode : public Event {
  EventNode() : m_next(NULL) {}

  EventNode *m_next;
};

struct TimeEvent : public Event {
  uint64_t m_repeat;
  uint64_t m_absTime;
  int64_t m_timeId;
};

struct SignalEvent : public EventNode {
  int m_signum;
};

struct IoEvent : public EventNode {
  int m_fd;
  int m_events;
};

struct ChildEvent : public Event {
  int m_pid;
  int m_status;
};

struct StatEvent : public Event {
  const char *m_path;
  std::string m_file;
  uint32_t m_mask;
  int m_wd;
};

struct AsyncEvent : Event {
  int m_send;
  int m_asyncId;
};

struct Anfd {
  Anfd() : m_head(NULL), m_events(0), m_reify(0), m_epollMask(0), m_cnt(0) {}

  void Reset() {
    m_head = NULL;
    m_reify = 0;
    m_cnt = 0;
    m_events = 0;
    m_reify = 0;
    m_epollMask = 0;
  }

  EventNode *m_head;
  unsigned char m_cnt;
  unsigned char m_events;    //注册的事件
  unsigned char m_reify;     //修改过的标记
  unsigned char m_epollMask; // epoll 的标记
};

struct Pending {
  Pending() : m_event(NULL), m_revents(0) {}

  Event *m_event;          // 对应事件
  unsigned char m_revents; //结果
};

typedef Event BeforeLoopEvent;
typedef Event AfterLoopEvent;

struct TimerHeap {
  void AddTimer(TimeEvent *event) {
    if (event == NULL || event->m_timeId != 0) {
      return;
    }
    m_timerHeap.push_back(event);
    HeapUp(m_timerHeap.size());
  }

  void RemoveTimer(TimeEvent *event) {
    if (event == NULL || event->m_timeId == 0) {
      return;
    }

    size_t nowIdx = event->m_timeId - 1;
    if (nowIdx >= m_timerHeap.size()) {
      return;
    }

    //设置 id 无效
    event->m_timeId = 0;

    // 放到 结尾
    TimeEvent *tail = m_timerHeap[m_timerHeap.size() - 1];

    std::swap(m_timerHeap[m_timerHeap.size() - 1], m_timerHeap[nowIdx]);
    m_timerHeap.pop_back();

    if (!m_timerHeap.empty()) {
      if (tail->m_absTime == event->m_absTime) {
        tail->m_timeId = nowIdx + 1;
      } else if (tail->m_absTime > event->m_absTime) {
        HeapDown(nowIdx); // nowIdx为当前位置
      }
    }
  }

  const int GetTimeout(uint64_t now) {
    if (m_timerHeap.empty()) {
      return -1;
    }

    int timeout = 0;
    TimeEvent *event = m_timerHeap.front();
    if (event->m_absTime >= now) {
      timeout = (int)(event->m_absTime - now);
    }
    return timeout;
  }

  TimeEvent *GetTimeoutEvent(uint64_t now) {
    if (m_timerHeap.empty()) {
      return NULL;
    }

    TimeEvent *event = m_timerHeap[0];
    if (event->m_absTime >= now) {
      return NULL;
    }

    std::swap(m_timerHeap[0], m_timerHeap[m_timerHeap.size() - 1]);
    m_timerHeap.pop_back();

    if (!m_timerHeap.size()) {
      HeapDown(0);
    }

    event->m_timeId = 0;
    return event;
  }

  const bool Empty() { return m_timerHeap.empty(); }

  const size_t Size() { return m_timerHeap.size(); }

private:
  /**
          从 endIdx] 一直向上 shift
  **/
  void HeapUp(const size_t endIdx) {
    size_t nowIdx = endIdx - 1;
    TimeEvent *event = m_timerHeap[nowIdx];

    int parentIdx = (nowIdx - 1) / 2;
    while (nowIdx > 0 && parentIdx >= 0 &&
           event->m_absTime < m_timerHeap[parentIdx]->m_absTime) {
      m_timerHeap[nowIdx] = m_timerHeap[parentIdx];
      m_timerHeap[nowIdx]->m_timeId = nowIdx + 1;

      nowIdx = parentIdx;
      parentIdx = (nowIdx - 1) / 2;
    }

    m_timerHeap[nowIdx] = event;
    event->m_timeId = nowIdx + 1;
  }

  /**
          从 [beginIdx 一直向下 shift
  **/
  void HeapDown(const size_t beginIdx) {
    size_t nowIdx = beginIdx;
    size_t childIdx = (nowIdx + 1) * 2;
    TimeEvent *obj = m_timerHeap[nowIdx];

    while (childIdx <= m_timerHeap.size()) {
      if (childIdx == m_timerHeap.size() ||
          m_timerHeap[childIdx - 1]->m_absTime <
              m_timerHeap[childIdx]->m_absTime) {
        childIdx -= 1;
      }

      TimeEvent *child = m_timerHeap[childIdx];
      if (obj->m_absTime <= child->m_absTime) {
        break;
      }

      m_timerHeap[nowIdx] = child;
      child->m_timeId = nowIdx + 1;

      nowIdx = childIdx;
      childIdx = (nowIdx + 1) * 2;
    }

    m_timerHeap[nowIdx] = obj;
    obj->m_timeId = nowIdx + 1;
  }

private:
  std::vector<TimeEvent *> m_timerHeap;
};

template <typename Type> struct Array {
public:
  Array() : m_max(32), m_cnt(0) { m_array = new Type[m_max](); }

  inline Type &operator[](size_t pos) {
    NeedArraySize(pos);
    if (pos >= m_cnt) {
      m_cnt = pos;
    }
    return m_array[pos];
  }

  inline const Type &operator[](size_t pos) const {
    NeedArraySize(pos);
    if (pos >= m_cnt) {
      m_cnt = pos;
    }
    return m_array[pos];
  }

  void Incr() { m_cnt += 1; }

  void Desc() { m_cnt -= 1; }

private:
  void NeedArraySize(int elemCnt) {
    if (elemCnt < m_max) {
      return;
    }

    int newMax = 1;
    do {
      newMax = newMax * 2;

    } while (elemCnt > newMax);

    Type *array = new Type[newMax]();
    for (int i = 0; i < m_max; ++i) {
      array[i] = m_array[i];
    }

    delete[] m_array;

    m_array = array;
    m_max = newMax;
  }

public:
  size_t m_cnt;

private:
  Type *m_array;
  int m_max;
};

struct EventLoop {
public:
  ~EventLoop();

  EventLoop(bool enableChild = true);

  void ExitLoop();

  void Dispath();

  void IoStart(IoEvent *ioEv);

  void IoStop(IoEvent *ioEv);

  void TimeStart(TimeEvent *timeEv);

  void TimeStop(TimeEvent *timeEv);

  void SignalStart(SignalEvent *sigEv);

  void SignalStop(SignalEvent *sigEv);

  void StartStat(StatEvent *statEv);

  void StopStat(StatEvent *statEv);

  void StartChild(ChildEvent *childEv);

  void StopChild(ChildEvent *childEv);

  void StartAsync(AsyncEvent *async);

  void StopAsync(AsyncEvent *async);

  void SendAsync(AsyncEvent *async);

  void StartBeforeLoop(BeforeLoopEvent *loopEvent);

  void StopBeforeLoop(BeforeLoopEvent *loopEvent);

  void StartAfterLoop(AfterLoopEvent *loopEvent);

  void StopAfterLoop(AfterLoopEvent *loopEvent);

private:
  void StartEvent(Event *event);

  void StopEvent(Event *event);

  void PollEpoll(int timeout);

  void ModifyEpoll(int fd, int oldEvents, int newEvents);

  void AddFdChanges(int fd);

  void ClearPending(Event *ev);

  void ReifyTimers();

  void ReifyFdChanges();

  void FeedEvent(Event *ev, int revents);

  void AddFdEvent(int fd, int revents);

  void AddSignalEvent(int signum);

  void AddChildEvent(int pid, int status);

  void AddStatEvent(inotify_event *inotifyEv);

  void InvokePendings();

private:
  void SignalFdProc(Event *event, int revent);
  void StatFdProc(Event *event, int revent);
  void ChildSigalProc(Event *event, int revent);
  void AsyncFdProc(Event *event, int revent);

public:
  bool m_enableChild;

  bool m_stop;
  int64_t m_now;
  int m_activeCnt;

  // epoll相关数据
  int m_epollFd;
  epoll_event *m_epollEvents;
  size_t m_epollEventMax;

  // io 事件
  std::vector<int> m_fdChanges;
  Array<Anfd> m_anfds;

  // 时间 事件
  TimerHeap m_timers;

  // 信号 事件
  sigset_t m_sigset;
  int m_sigFd;
  IoEvent m_sigFdEvent;
  EventProc m_sigFdProc;
  EventNode *m_signals[128];

  // 子进程退出 事件
  SignalEvent m_childEvent;
  std::multimap<pid_t, ChildEvent *> m_childs;
  typedef std::multimap<pid_t, ChildEvent *>::iterator ChildMapIt;

  // 监控 文件 事件
  int m_notifyFd;
  IoEvent m_statEvent;
  std::multimap<int, StatEvent *> m_stats;
  typedef std::multimap<int, StatEvent *>::iterator StatMapIt;

  // Loop 前后事件
  std::vector<BeforeLoopEvent *> m_beforeEvent;
  std::vector<AfterLoopEvent *> m_afterEvent;

  // async
  int m_eventFd;
  int m_eventPipe[2];
  IoEvent m_asyncEv;
  std::vector<AsyncEvent *> m_asyncs;

  // 待处理事件
  Array<Pending> m_pendings[kHighPriority + 1];
};

static void AddNodeToList(EventNode **node, EventNode *elem) {
  elem->m_next = *node;
  *node = elem;
}

static void DelNodeList(EventNode **head, EventNode *elem) {
  EventNode *cur = *head;
  EventNode *prev = NULL;
  while (cur) {
    if (cur == elem) {
      if (*head == elem) {
        *head = elem->m_next;
      } else {
        prev->m_next = elem->m_next;
      }
      break;
    }
    prev = cur;
    cur = cur->m_next;
  }
}

// ms
uint64_t GetNow() {
  timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void TimerInit(TimeEvent *ev, EventProc proc, uint64_t tv,
               uint64_t repeat = 0) {
  ev->m_active = 0;
  ev->m_priority = kHighPriority;
  ev->m_pending = 0;
  ev->m_repeat = repeat < 0 ? 0 : repeat;
  ev->m_absTime = tv + GetNow();
  ev->m_timeId = 0;
  ev->m_proc = proc;
}

void TimerInit(TimeEvent *ev, EventProc proc, timeval tv) {
  TimerInit(ev, proc, tv.tv_sec * 1000 + tv.tv_usec / 1000, 0);
}

void TimerInit(TimeEvent *ev, EventProc proc, timeval tv, timeval repeat) {
  TimerInit(ev, proc, tv.tv_sec * 1000 + tv.tv_usec / 1000,
            repeat.tv_sec * 1000 + repeat.tv_usec / 1000);
}

void SignalInit(SignalEvent *ev, EventProc proc, int signum) {
  ev->m_active = 0;
  ev->m_priority = kHighPriority;
  ev->m_pending = 0;
  ev->m_signum = signum;
  ev->m_proc = proc;
}

void AsyncInit(AsyncEvent *ev) {
  ev->m_active = 0;
  ev->m_priority = kHighPriority;
  ev->m_pending = 0;
  ev->m_asyncId = 0;
  ev->m_send = 0;
  ev->m_proc = proc;
}

void IoInit(IoEvent *ev, EventProc proc, int fd, int events) {
  ev->m_active = 0;
  ev->m_priority = kLowPriority;
  ev->m_pending = 0;
  ev->m_fd = fd;
  ev->m_events = events;
  ev->m_proc = proc;
}

void StatInit(StatEvent *ev, EventProc proc, const char *path) {
  ev->m_active = 0;
  ev->m_priority = kHighPriority;
  ev->m_pending = 0;
  ev->m_path = path;
  ev->m_mask = -1;
  ev->m_wd = -1;
  ev->m_proc = proc;
}

void ChildInit(ChildEvent *ev, EventProc proc, int pid) {
  ev->m_active = 0;
  ev->m_priority = kHighPriority;
  ev->m_pending = 0;
  ev->m_pid = pid;
  ev->m_status = 0;
  ev->m_proc = proc;
}

void EventInit(Event *ev, EventProc proc) {
  ev->m_active = 0;
  ev->m_priority = kLowPriority;
  ev->m_pending = 0;
  ev->m_proc = proc;
}

#endif
