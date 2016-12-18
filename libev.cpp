
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/eventfd.h>

#include <stdio.h>
#include <ucontext.h>

#include <string>
#include <iostream>
#include <sstream>

#include "libev.h"

void EventLoop::SignalFdProc(Event *event, int revent) {
  signalfd_siginfo si[2], *sip;
  while (1) {
    ssize_t res = read(m_sigFd, si, sizeof(si));

    for (sip = si; (char *)sip < (char *)si + res; ++sip) {
      AddSignalEvent(sip->ssi_signo);
    }

    if (res < (ssize_t)sizeof(si)) {
      break;
    }
  }
}

void EventLoop::AsyncFdProc(Event *event, int revent) {
  if (revent & kReadEvent) {
    if (m_eventFd >= 0) {
      uint64_t counter;
      read(m_eventFd, &counter, sizeof(uint64_t));
    } else if (m_eventPipe[0] >= 0) {
      char dummy;
      read(m_eventPipe[0], &dummy, sizeof(char));
    }

    for (int i = 0; i < m_asyncs.size(); ++i) {
      if (m_asyncs[i]->m_send == 1) {
        m_asyncs[i]->m_send = 0;
        FeedEvent(m_asyncs[i], kAsyncEvent);
      }
    }
  }
}

void EventLoop::StatFdProc(Event *event, int revent) {
  const int bufSize = (sizeof(inotify_event) + NAME_MAX);
  char buf[bufSize];
  int len = read(m_notifyFd, buf, bufSize);
  for (int i = 0; i < len;) {
    inotify_event *ev = (inotify_event *)(&buf[i]);
    if (ev->len > 0) {
      AddStatEvent(ev);
    }
    i += sizeof(inotify_event) + ev->len;
  }
}

void EventLoop::ChildSigalProc(Event *event, int revent) {
  int pid, status;
  while (1) {
    pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
    if (pid <= 0) {
      break;
    } else {
      AddChildEvent(pid, status);
    }
  }
}

EventLoop::EventLoop(bool enableChild)
    : m_stop(true), m_enableChild(enableChild), m_now(GetNow()), m_activeCnt(0),
      m_epollFd(-1), m_epollEvents(NULL), m_epollEventMax(0), m_sigFd(-1),
      m_notifyFd(-1), m_eventFd(-1) {

  m_eventPipe[0] = m_eventPipe[1] = -1;

  for (int i = 0; i < 128; ++i) {
    m_signals[i] = NULL;
  }

  m_epollFd = epoll_create(256);

  fcntl(m_epollFd, F_SETFD, FD_CLOEXEC);

  m_epollEventMax = 64;
  m_epollEvents = new epoll_event[m_epollEventMax];

  if (m_enableChild) {
    SignalInit(&m_childEvent, (EventProc)&EventLoop::ChildSigalProc, SIGCHLD);
    m_childEvent.m_priority = kHighPriority;

    SignalStart(&m_childEvent);
    m_activeCnt = m_activeCnt - 1;
  }
}

EventLoop::~EventLoop() {
  close(m_epollFd);
  delete m_epollEvents;

  if (m_sigFd > 0) {
    close(m_sigFd);
  }

  if (m_notifyFd > 0) {
    close(m_notifyFd);
  }
}

void EventLoop::ExitLoop() { m_stop = true; }

void EventLoop::Dispath() {
  m_stop = false;

  do {

    // Loop 之前
    for (int i = 0; i < m_afterEvent.size(); ++i) {
      FeedEvent(m_afterEvent[i], kAfterLoop);
    }

    if (m_stop) {
      break;
    }
    // 修改 io 事件
    ReifyFdChanges();

    uint64_t now = GetNow();
    // 超时
    int timeout = m_timers.GetTimeout(now);

    timeout = timeout < 0 ? -1 : timeout;

    // epoll
    PollEpoll(timeout);

    // timers
    ReifyTimers();

    // Loop之后

    for (int i = 0; i < m_beforeEvent.size(); ++i) {
      FeedEvent(m_beforeEvent[i], kBeforeLoop);
    }
    // Invoke Pending
    InvokePendings();
  } while (!m_stop && m_activeCnt > 0);
}

void EventLoop::IoStart(IoEvent *ioEv) {
  if (ioEv->m_active) {
    return;
  }
  StartEvent(ioEv);

  AddNodeToList(&m_anfds[ioEv->m_fd].m_head, ioEv);
  m_anfds[ioEv->m_fd].m_cnt += 1;

  AddFdChanges(ioEv->m_fd);
}

void EventLoop::IoStop(IoEvent *ioEv) {
  ClearPending(ioEv);

  if (!ioEv->m_active) {
    return;
  }

  DelNodeList(&m_anfds[ioEv->m_fd].m_head, ioEv);
  m_anfds[ioEv->m_fd].m_cnt -= 1;

  StopEvent(ioEv);
  AddFdChanges(ioEv->m_fd);
}

void EventLoop::TimeStart(TimeEvent *timeEv) {
  if (timeEv->m_active) {
    return;
  }

  StartEvent(timeEv);
  m_timers.AddTimer(timeEv);
}

void EventLoop::TimeStop(TimeEvent *timeEv) {
  ClearPending(timeEv);

  if (!timeEv->m_active) {
    return;
  }

  StopEvent(timeEv);
  m_timers.RemoveTimer(timeEv);
}

void EventLoop::SignalStart(SignalEvent *sigEv) {
  if (sigEv->m_active) {
    return;
  }

  if (m_sigFd < 0) {
    m_sigFd = signalfd(-1, &m_sigset, SFD_NONBLOCK | SFD_CLOEXEC);
    if (m_sigFd < 0 && errno == EINVAL) {
      m_sigFd = signalfd(-1, &m_sigset, 0);
    }

    if (m_sigFd >= 0) {
      fcntl(m_sigFd, F_SETFD, FD_CLOEXEC);
      fcntl(m_sigFd, F_SETFL, O_NONBLOCK);

      sigemptyset(&m_sigset);
      m_sigFdEvent.m_priority = kHighPriority;
      IoInit(&m_sigFdEvent, (EventProc)&EventLoop::SignalFdProc, m_sigFd,
             kReadEvent);
      IoStart(&m_sigFdEvent);
      m_activeCnt -= 1;
    }
  }

  if (m_sigFd >= 0) {
    sigaddset(&m_sigset, sigEv->m_signum);
    sigprocmask(SIG_BLOCK, &m_sigset, 0);
    signalfd(m_sigFd, &m_sigset, 0);

    StartEvent(sigEv);
    AddNodeToList(&m_signals[sigEv->m_signum - 1], sigEv);
  }
}

void EventLoop::SignalStop(SignalEvent *sigEv) {
  ClearPending(sigEv);
  if (!sigEv->m_active) {
    return;
  }

  if (m_signals[sigEv->m_signum - 1] == NULL) {
    if (m_sigFd >= 0) {
      sigset_t sigset;
      sigemptyset(&sigset);

      sigaddset(&sigset, sigEv->m_signum);
      sigdelset(&m_sigset, sigEv->m_signum);

      signalfd(m_sigFd, &m_sigset, 0);
      sigprocmask(SIG_UNBLOCK, &sigset, 0);

    } else {

      signal(sigEv->m_signum, SIG_DFL);
    }
  }

  DelNodeList(&m_signals[sigEv->m_signum - 1], sigEv);
  StopEvent(sigEv);
}

void EventLoop::SendAsync(AsyncEvent *async) {
  if (async->m_send == 1) {
    return;
  }

  if (m_eventFd >= 0) {
    async->m_send = 1;
    uint64_t counter;
    write(m_eventFd, &counter, sizeof(uint64_t));
  } else if (m_eventPipe[1] > 0) {
    async->m_send = 1;
    char dummy;
    write(m_eventPipe[1], &dummy, sizeof(char));
  }
}

void EventLoop::StopAsync(AsyncEvent *async) {
  ClearPending(async);
  if (!async->m_active) {
    return;
  }

  int position = async->m_asyncId - 1;
  if (position >= m_asyncs.size()) {
    return;
  }

  std::swap(m_asyncs[position], m_asyncs[m_asyncs.size() - 1]);
  m_asyncs.pop_back();

  async->m_asyncId = 0;
  if (!m_asyncs.empty()) {
    m_asyncs[position]->m_asyncId = position;
  }
  StopEvent(async);
}

void EventLoop::StartAsync(AsyncEvent *async) {
  if (async->m_active) {
    return;
  }

  if (async->m_asyncId > 0) {
    return;
  }

  if (m_eventFd < 0) {
    m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_eventFd < 0 && errno == EINVAL) {
      m_eventFd = eventfd(0, 0);
    }

    if (m_eventFd >= 0) {
      IoInit(&m_asyncEv, (EventProc)&EventLoop::AsyncFdProc, m_eventFd,
             kReadEvent);
      IoStart(&m_asyncEv);
      m_activeCnt = m_activeCnt - 1;

    } else {

      pipe(m_eventPipe);
      fcntl(m_eventPipe[0], F_SETFD, FD_CLOEXEC);
      fcntl(m_eventPipe[0], F_SETFL, O_NONBLOCK);

      fcntl(m_eventPipe[1], F_SETFD, FD_CLOEXEC);
      fcntl(m_eventPipe[1], F_SETFL, O_NONBLOCK);

      IoInit(&m_asyncEv, (EventProc)&EventLoop::AsyncFdProc, m_eventPipe[0],
             kReadEvent);
      IoStart(&m_asyncEv);
      m_activeCnt = m_activeCnt - 1;
    }
  }

  async->m_asyncId = m_asyncs.size();
  m_asyncs.push_back(async);

  StartEvent(async);
}

void EventLoop::StartStat(StatEvent *statEv) {
  if (statEv->m_active) {
    return;
  }

  const char *path = statEv->m_path;
  struct stat buf;
  if (path == NULL || ::stat(path, &buf) != 0) {
    return;
  }
  if (m_notifyFd < 0) {
    m_notifyFd = inotify_init();

    if (m_notifyFd >= 0) {
      fcntl(m_notifyFd, F_SETFD, FD_CLOEXEC);
      fcntl(m_notifyFd, F_SETFL, O_NONBLOCK);

      m_statEvent.m_priority = kHighPriority;
      IoInit(&m_statEvent, (EventProc)&EventLoop::StatFdProc, m_notifyFd,
             kReadEvent);
      IoStart(&m_statEvent);
      m_activeCnt -= 1;
    }
  }

  if (m_notifyFd >= 0) {
    statEv->m_wd = inotify_add_watch(m_notifyFd, path,
                                     IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF |
                                         IN_MODIFY | IN_CREATE | IN_DELETE |
                                         IN_MOVED_FROM | IN_MOVED_TO |
                                         IN_DONT_FOLLOW | IN_MASK_ADD);
    StartEvent(statEv);
    m_stats.insert(std::make_pair(statEv->m_wd, statEv));
  }
}

void EventLoop::StopStat(StatEvent *statEv) {
  ClearPending(statEv);
  if (!statEv->m_active) {
    return;
  }

  std::pair<StatMapIt, StatMapIt> pos = m_stats.equal_range(statEv->m_wd);
  while (pos.first != pos.second) {
    if (pos.first->second == statEv) {
      m_stats.erase(pos.first);
    } else {
      ++pos.first;
    }
  }

  inotify_rm_watch(m_notifyFd, statEv->m_wd);
  StopEvent(statEv);
}

void EventLoop::StartChild(ChildEvent *childEv) {
  if (childEv->m_active) {
    return;
  }

  StartEvent(childEv);
  m_childs.insert(std::make_pair(childEv->m_pid, childEv));
}

void EventLoop::StopChild(ChildEvent *childEv) {
  ClearPending(childEv);
  if (!childEv->m_active) {
    return;
  }

  std::pair<ChildMapIt, ChildMapIt> pos = m_childs.equal_range(childEv->m_pid);
  while (pos.first != pos.second) {
    if (pos.first->second == childEv) {
      m_childs.erase(pos.first);
    } else {
      ++pos.first;
    }
  }

  StopEvent(childEv);
}

void EventLoop::StartBeforeLoop(BeforeLoopEvent *loopEvent) {
  if (loopEvent->m_active) {
    return;
  }
  loopEvent->m_priority = kHighPriority;
  m_beforeEvent.push_back(loopEvent);
  StartEvent(loopEvent);
}

void EventLoop::StopBeforeLoop(BeforeLoopEvent *loopEvent) {
  ClearPending(loopEvent);
  if (!loopEvent->m_active) {
    return;
  }

  for (int i = 0; i < m_beforeEvent.size(); ++i) {
    if (m_beforeEvent[i] == loopEvent) {
      m_beforeEvent.erase(m_beforeEvent.begin() + i);
      break;
    }
  }
  StopEvent(loopEvent);
}

void EventLoop::StartAfterLoop(AfterLoopEvent *loopEvent) {
  if (loopEvent->m_active) {
    return;
  }
  loopEvent->m_priority = kLowPriority;
  m_afterEvent.push_back(loopEvent);
  StartEvent(loopEvent);
}

void EventLoop::StopAfterLoop(AfterLoopEvent *loopEvent) {
  ClearPending(loopEvent);
  if (!loopEvent->m_active) {
    return;
  }

  for (int i = 0; i < m_afterEvent.size(); ++i) {
    if (m_afterEvent[i] == loopEvent) {
      m_afterEvent.erase(m_afterEvent.begin() + i);
      break;
    }
  }
  StopEvent(loopEvent);
}

void EventLoop::StartEvent(Event *event) {
  event->m_active = 1;
  m_activeCnt += 1;
}

void EventLoop::StopEvent(Event *event) {
  event->m_active = 0;
  m_activeCnt -= 1;
}

void EventLoop::AddFdChanges(int fd) {
  if (m_anfds[fd].m_reify == 0) {
    // m_reify = 1, 要 ReifyFdChanges之前已经修改过
    m_fdChanges.push_back(fd);
    m_anfds[fd].m_reify = 1;
  }
}

void EventLoop::ReifyFdChanges() {
  for (int i = 0; i < m_fdChanges.size(); ++i) {
    int fd = m_fdChanges[i];
    Anfd &anfd = m_anfds[fd];

    unsigned char old_events = anfd.m_events;
    unsigned char old_reify = anfd.m_reify;
    anfd.m_reify = 0; // 置为未修改

    if (old_reify) // 有修改过，则算一下现在的events值
    {
      for (EventNode *cur = anfd.m_head; cur != NULL; cur = cur->m_next) {
        IoEvent *ioEv = (IoEvent *)cur;

        anfd.m_events |= ioEv->m_events;
      }

      old_reify =
          (old_events != anfd.m_events ? 1 : 0); // 之前修改和 现在是否一样
    }

    if (old_reify == 1) {
      ModifyEpoll(fd, old_events, anfd.m_events); //添加或修改为新事件
    }

    if (m_anfds[fd].m_cnt == 0) {
      m_anfds[fd].Reset();
    }
  }
  m_fdChanges.clear();
}

void EventLoop::ClearPending(Event *ev) {
  if (ev->m_pending > 0) {
    int pri = ev->m_priority - 1;
    Pending &pending = m_pendings[pri][ev->m_pending - 1];
    pending.m_event == NULL;
    pending.m_revents = 0;
    ev->m_pending = 0;
  }
}

void EventLoop::PollEpoll(int timeout) {
  int eventCnt = epoll_wait(m_epollFd, m_epollEvents, m_epollEventMax, timeout);

  if (eventCnt < 0 && errno != EINTR) {
    return;
  }

  for (int i = 0; i < eventCnt; ++i) {
    epoll_event *ev = &m_epollEvents[i];

    int fd = (int)ev->data.u64;
    int want = m_anfds[fd].m_events;

    int got = (ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP) ? kReadEvent : 0) |
              (ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP) ? kWriteEvent : 0);

    if ((got & ~want) > 0) {

      ev->events = (want & kReadEvent ? EPOLLIN : 0) |
                   (want & kWriteEvent ? EPOLLOUT : 0);

      if (epoll_ctl(m_epollFd, want ? EPOLL_CTL_MOD : EPOLL_CTL_DEL, fd, ev)) {
        continue;
      }
    }

    AddFdEvent(fd, got);
  }

  // 扩大 epoll_events
  if (eventCnt == m_epollEventMax) {
    delete[] m_epollEvents;
    m_epollEventMax = m_epollEventMax * 2;
    m_epollEvents = new epoll_event[m_epollEventMax];
  }
}

void EventLoop::ModifyEpoll(int fd, int oldEvents, int newEvents) {
  if (oldEvents == newEvents) {
    return;
  }

  unsigned char oldMask = m_anfds[fd].m_epollMask;

  epoll_event ev;
  ev.data.u64 = fd;
  ev.events = (newEvents & kReadEvent) ? EPOLLIN : 0 | (newEvents & kWriteEvent)
                                                       ? EPOLLOUT
                                                       : 0;

  // 旧与新的不一样，需要修改 epoll
  if (oldMask != ev.events) {
    bool optRet = false;
    do {
      if (epoll_ctl(m_epollFd,
                    (!oldMask && newEvents) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd,
                    &ev) == 0) {
        optRet = true;
        break;
      }

      if (errno == ENOENT) // 出错, 重试一次
      {
        if (epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) == 0) {
          optRet = true;
          break;
        }

      } else if (errno == EEXIST) {
        if (!epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) == 0) {
          optRet = true;
          break;
        }
      }

    } while (0);

    if (optRet) {
      m_anfds[fd].m_epollMask = ev.events;
    }
  }
}

void EventLoop::ReifyTimers() {
  uint64_t now = GetNow();
  TimeEvent *timeEv = NULL;
  while (1) {
    timeEv = m_timers.GetTimeoutEvent(now);
    if (timeEv != NULL) {
      FeedEvent(timeEv, kTimeoutEvent);
      if (timeEv->m_repeat > 0) {
        timeEv->m_absTime = timeEv->m_repeat + GetNow();
        m_timers.AddTimer(timeEv);
      } else {
        StopEvent(timeEv);
      }
    } else {
      break;
    }
  }
}

void EventLoop::FeedEvent(Event *ev, int revents) {
  int pri = ev->m_priority - 1;
  if (ev->m_pending > 0) // 事件已经发生
  {
    m_pendings[pri][ev->m_pending - 1].m_revents |= revents;

  } else {
    ev->m_pending = m_pendings[pri].m_cnt + 1;
    Pending &pending = m_pendings[pri][ev->m_pending - 1];
    pending.m_event = ev;
    pending.m_revents = revents;
    m_pendings[pri].m_cnt += 1;
  }
}

void EventLoop::AddFdEvent(int fd, int revents) {
  Anfd &anfd = m_anfds[fd];
  for (EventNode *event = anfd.m_head; event != NULL; event = event->m_next) {
    IoEvent *ioEv = (IoEvent *)event;
    int ev = ioEv->m_events & revents;
    if (ev) {
      FeedEvent(ioEv, ev);
    }
  }
}

void EventLoop::AddSignalEvent(int signum) {
  SignalEvent *sigEv = (SignalEvent *)m_signals[signum - 1];
  if (sigEv != NULL) {
    for (EventNode *cur = sigEv; cur != NULL; cur = cur->m_next) {
      FeedEvent(cur, kSignalEvent);
    }
  }
}

void EventLoop::AddChildEvent(int pid, int status) {

  std::pair<ChildMapIt, ChildMapIt> pos = m_childs.equal_range(pid);
  while (pos.first != pos.second) {
    ChildEvent *childEv = pos.first->second;

    childEv->m_priority = kHighPriority;
    childEv->m_pid = pid;
    FeedEvent(childEv, kChildEvent);

    ++pos.first;
  }
}

void EventLoop::AddStatEvent(inotify_event *inotifyEv) {
  std::string file(inotifyEv->name);
  uint32_t mask = inotifyEv->mask;
  int wd = inotifyEv->wd;

  std::pair<StatMapIt, StatMapIt> pos = m_stats.equal_range(wd);
  while (pos.first != pos.second) {
    StatEvent *statEv = pos.first->second;
    statEv->m_mask = mask;

    if (statEv->m_wd == wd || wd == -1) {
      if (mask & (IN_IGNORED | IN_UNMOUNT | IN_DELETE_SELF)) {
        //删除
        inotify_rm_watch(m_notifyFd, wd);

        // 重新添加
        int addMask = IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_MODIFY |
                      IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                      IN_DONT_FOLLOW | IN_MASK_ADD;
        statEv->m_wd = inotify_add_watch(m_notifyFd, statEv->m_path, addMask);
      }
      statEv->m_file = file;
      FeedEvent(statEv, kStatEvent);
    }

    ++pos.first;
  }
}

void EventLoop::InvokePendings() {
  for (int pri = kHighPriority - 1; pri >= kLowPriority - 1; --pri) {
    Array<Pending> &pendings = m_pendings[pri];

    while (pendings.m_cnt > 0) {
      Pending pending = pendings[pendings.m_cnt - 1];
      pendings.m_cnt -= 1;

      if (pending.m_event != NULL && pending.m_event->m_pending > 0) {
        pending.m_event->m_pending = 0;
        pending.m_event->m_proc(this, pending.m_event, pending.m_revents);
      }
    }
  }
}

static void StartUThread(uint32_t low32, uint32_t high32);
struct UThread;
typedef void (*UThreadProc)(UThread *ut, void *args);
struct UThread {
public:
  UThread(UThreadProc proc, void *data, int sizeStack = 1024 * 1024) {
    UThread *ut;
    m_stack = new unsigned char[sizeStack];
    m_stackSize = sizeStack;
    m_active = 0;
    m_exitCode = 0;
    m_proc = proc;
    m_data = data;
    m_active = 1;

    memset(&m_context, 0, sizeof(ucontext_t));

    sigset_t zero;
    sigemptyset(&zero);
    sigprocmask(SIG_BLOCK, &zero, &m_context.uc_sigmask);

    getcontext(&m_context);
    m_context.uc_link = 0;
    m_context.uc_stack.ss_sp = m_stack;
    m_context.uc_stack.ss_size = m_stackSize;
    m_context.uc_link = &m_scheduleContext;
    makecontext(&m_context, (void (*)())StartUThread, 2,
                (uint32_t)uintptr_t(this), (uint32_t)(uintptr_t(this) >> 32));
  }

  ~UThread() { delete[] m_stack; }

  void Yield() { swapcontext(&m_context, &m_scheduleContext); }

  void Resume() { swapcontext(&m_scheduleContext, &m_context); }

  void Exit() {
    m_active = 0;
    Yield();
  }

  ucontext_t m_scheduleContext;
  ucontext_t m_context;

  unsigned char *m_stack;
  uint32_t m_stackSize;
  int m_active;
  int m_exitCode;

  UThreadProc m_proc;
  void *m_data;
};

static void StartUThread(uint32_t low32, uint32_t high32) {
  uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)high32 << 32);
  UThread *th = (UThread *)(ptr);
  th->m_proc(th, th->m_data);
  th->Exit();
}

void TaskSchedule(UThread *ut) {
  ut->Resume();
  if (ut->m_active == 0) {
    delete ut;
  }
}

template <typename Type> struct FdMap {
public:
  FdMap() {
    m_entry = NULL;
    m_entryCnt = 0;
  }

  ~FdMap() {
    if (m_entry != NULL) {
      delete[] m_entry;
      m_entry = NULL;
    }
    m_entryCnt = 0;
  }

  bool AddFd(int fd, Type *p) {
    if (fd < 0) {
      return false;
    }

    NeedSize(fd);

    m_entry[fd] = p;

    return true;
  }

  bool DelFd(int fd) {
    if (fd < 0) {
      return false;
    }

    if (m_entryCnt > fd) {
      m_entry[fd] = NULL;
    }
    return false;
  }

  Type *GetFd(int fd) {
    if (fd < 0 && fd > m_entryCnt) {
      return NULL;
    }
    return m_entry[fd];
  }

private:
  void NeedSize(int slot) {
    if (m_entryCnt <= slot) {
      int cnt = (m_entry == NULL ? 32 : m_entryCnt);

      while (cnt <= slot) {
        cnt = cnt << 1;
      }
      Type **entry = new Type *[cnt];

      for (int i = 0; i < m_entryCnt; ++i) {
        entry[i] = m_entry[i];
      }

      m_entryCnt = cnt;
      m_entry = entry;
    }
  }

private:
  Type **m_entry;
  int m_entryCnt;
};

struct EventLoopUThread;
struct SocketUThread {
  int Accept(int fd, struct sockaddr *cliAddr, socklen_t *addrLen);

  // 调用方知道 需要写多少，要不断地写数据
  int Write(int fd, const void *buf, size_t count);

  // 调用方不知道有多少数据
  int Read(int fd, void *buf, size_t count);

  EventLoopUThread *m_loopUt;
  UThread *m_ut;

  int m_yieldFd;    
  int m_yieldEvent;   
  int m_currentEvent; 
};

typedef void (*SocketUThreadProc)(SocketUThread *sut, int fd, void *data);
void SocketUthreadReadProc(EventLoop *loop, Event *event, int revents);
void SocketUthreadWriteProc(EventLoop *loop, Event *event, int revents);

struct SocketUTData {
  SocketUThread *m_st;
  SocketUThreadProc m_proc;
  int m_fd;
  void *m_data;
};

void SocketUthreadSpawnProc(UThread *ut, void *data);

struct EventLoopUThread {
public:
  EventLoopUThread(EventLoop *loop) : m_ioMap(), m_loop(loop) {}

  int SpawnWithFd(int fd, SocketUThreadProc proc, void *data) {

    SocketUTData *utData = new SocketUTData();
    SocketUThread *st = new SocketUThread();
    st->m_loopUt = this;
    st->m_ut = new UThread(SocketUthreadSpawnProc, utData);

    utData->m_proc = proc;
    utData->m_fd = fd;
    utData->m_data = data;
    utData->m_st = st;
    GreenFd(st, fd);

    TaskSchedule(st->m_ut); //可能会删掉 ut
  }

  int Spawn(SocketUThreadProc proc, void *data) {
    SocketUTData *utData = new SocketUTData();
    SocketUThread *st = new SocketUThread();
    st->m_loopUt = this;
    st->m_ut = new UThread(SocketUthreadSpawnProc, utData); //都设置了

    utData->m_proc = proc;
    utData->m_fd = -1;
    utData->m_data = data;
    utData->m_st = st;

    TaskSchedule(st->m_ut);
  }

  void GreenFd(SocketUThread *st, int fd) {
    if (fd < 0) {
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
    }
    IoEvent *ev = new IoEvent();
    ev->m_data = st;
    IoInit(ev, SocketUthreadReadProc, fd, kReadEvent);
    m_loop->IoStart(ev);
    m_ioMap.AddFd(fd, ev);
  }

  void UnGreenFd(int fd) {
    if (fd < 0) {
    }
    IoEvent *ev = m_ioMap.GetFd(fd);

    if (ev != NULL) {
      m_loop->IoStop(ev);
      delete ev;
      m_ioMap.DelFd(fd);
    }
  }

  EventLoop *m_loop;
  FdMap<IoEvent> m_ioMap; //管理 ev
};

// 调用方不知道有多少数据
int SocketUThread::Read(int fd, void *buf, size_t count) {
  int readBytes = 0;
  while (1) {
    readBytes = read(fd, buf, count);

    if (readBytes >= 0) {
      break;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN) {
      m_yieldFd = fd;
      m_yieldEvent = kReadEvent;
      m_ut->Yield();
      continue;
    }
    break;
  }
  return readBytes;
}

int SocketUThread::Accept(int fd, struct sockaddr *cliAddr,
                          socklen_t *addrLen) {
  int connFd = -1;
  while (1) {
    connFd = accept(fd, cliAddr, addrLen);
    if (connFd < 0) {
      if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN) {
        this->m_yieldFd = fd;
        this->m_yieldEvent = kReadEvent;
        m_ut->Yield();
        continue;
      }
    }
    break;
  }
  return connFd;
}

// 调用方知道 需要写多少，要不断地写数据
int SocketUThread::Write(int fd, const void *buf, size_t count) {
  int left = count;
  int offset = 0;
  IoEvent *writeEv = NULL;

  while (left > 0) {
    int ret = write(fd, (char *)buf + offset, left);
    if (ret <= 0) {
      if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN) {
        if (writeEv == NULL) {
          writeEv = new IoEvent;
          writeEv->m_data = this;
          IoInit(writeEv, SocketUthreadWriteProc, fd, kWriteEvent);
          m_loopUt->m_loop->IoStart(writeEv);
        }

        this->m_yieldFd = fd;
        this->m_yieldEvent = kWriteEvent;
        m_ut->Yield();
        continue;
      } else {
        break;
      }
    }
    left = left - ret;
    offset = offset + ret;
  }

  if (writeEv != NULL) {
    m_loopUt->m_loop->IoStop(writeEv);
    delete writeEv;
  }
  return left <= 0 ? count : offset;
}

void SocketUthreadSpawnProc(UThread *ut, void *data) {
  SocketUTData *utData = (SocketUTData *)data;
  utData->m_st->m_ut = ut;
  utData->m_proc(utData->m_st, utData->m_fd, utData->m_data);

  if (utData->m_fd > 0) {
    EventLoopUThread *loopUt = utData->m_st->m_loopUt;
    loopUt->UnGreenFd(utData->m_fd);
  }
  delete utData;
}

void SocketUthreadReadProc(EventLoop *loop, Event *event, int revents) {
  IoEvent *ev = (IoEvent *)event;
  SocketUThread *st = (SocketUThread *)event->m_data;
  if (st->m_yieldFd == ev->m_fd && revents & kReadEvent) {
    st->m_ut->Resume();
    if (st->m_ut->m_active == 0) {
      delete st->m_ut;
    }
  }
}

void SocketUthreadWriteProc(EventLoop *loop, Event *event, int revents) {
  IoEvent *ev = (IoEvent *)event;
  SocketUThread *st = (SocketUThread *)event->m_data;
  if (st->m_yieldFd == ev->m_fd && revents & kWriteEvent) {
    st->m_ut->Resume();
    if (st->m_ut->m_active == 0) {
      delete st->m_ut;
    }
  }
}

// -------------libev + uthread -> one thread on loop
// example--------------------
int GetListenFd(short port = 8080) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in serv_addr;
  bzero(&serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  int option = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&option, sizeof(option));

  struct linger li;
  li.l_onoff = 1;
  li.l_linger = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (char *)&li, sizeof(li));

  bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
  listen(sockfd, 10);
  return sockfd;
}

class Buffer {
public:
  Buffer() : m_used(0), m_pos(0) { NeedBufferSize(16 * 1024); }

  void NeedBufferSize(size_t len) {
    if (len < m_data.size()) {
      return;
    }
    m_data.resize(len * 2);
  }

  void *Data() { return (char *)m_data.data(); }

  size_t UsedLength() { return m_used; }

  void *UnusedData() { return (char *)Data() + m_used; }

  size_t UnusedLength() { return m_data.size() - m_used; }

  void AddUsed(size_t size) {
    m_used += size;
    NeedBufferSize(m_used);
  }

  std::string m_data;
  size_t m_used;
  size_t m_pos;
};

struct Socket {
public:
  Socket(int fd) : m_fd(fd) {}

  Buffer m_recvBuffer;
  Buffer m_sendBuffer;
  int m_fd;
};

class HttpRequest {
public:
  void Parse(std::string &str) {
    std::vector<std::string> header = Split(str, "\r\n");
    for (int i = 0; i < header.size(); ++i) {
      std::vector<std::string> kv = Split(header[i], ":");
      if (kv.size() == 2) {
        m_headers[kv[0]] = kv[1];
      }

      if (header[i].find_first_of("GET") == 0) {
        m_path = header[i].substr(header[i].find_first_of('/'));
        m_path = m_path.substr(0, m_path.find_first_of(" "));
        m_path = m_path.substr(0, m_path.find_first_of("?"));
        std::string query =
            header[i].substr(header[i].find_first_of(' '), header[i].length());
        if (query.find_first_of('?') != std::string::npos) {
          query = query.substr(query.find_first_of('?') + 1);
          query = query.substr(0, query.find_first_of("HTTP"));
          std::vector<std::string> querys = Split(query, "&");

          for (int i = 0; i < querys.size(); ++i) {
            std::vector<std::string> kv = Split(querys[i], "=");
            if (kv.size() == 2) {
              m_query[kv[0]] = kv[1];
            }
          }
        }
      }
    }
  }

  std::string &GetParam(std::string &key) { return m_query[key]; }

private:
  std::vector<std::string> Split(const std::string &src,
                                 const std::string &separator) {
    std::vector<std::string> dest;
    std::string str = src;
    std::string substring;
    std::string::size_type start = 0, index;

    do {
      index = str.find_first_of(separator, start);
      if (index != std::string::npos) {
        substring = str.substr(start, index - start);
        dest.push_back(substring);
        start = str.find_first_not_of(separator, index);
        if (start == std::string::npos)
          break;
      }
    } while (index != std::string::npos);

    // the last token
    substring = str.substr(start);
    dest.push_back(substring);
    return dest;
  }

public:
  std::map<std::string, std::string> m_query;
  std::map<std::string, std::string> m_headers;
  std::string m_body;
  std::string m_path;
};

class HttpResponse {
public:
  HttpResponse() { AddHeader("Server", "HttpServer"); }

  std::string AsString() {
    std::stringstream ss;
    ss << "HTTP/1.1 200 OK\r\n";
    std::map<std::string, std::string>::iterator it = m_headers.begin();
    for (; it != m_headers.end(); ++it) {
      ss << it->first << ": " << it->second << "\r\n";
    }
    ss << "\r\n" << m_body;
    // printf("%s\n", ss.str().c_str() );
    return ss.str();
  }

public:
  void AddHeader(const std::string &key, const std::string &value) {
    m_headers[key] = value;
  }

  void AddBody(const std::string &body) {
    std::stringstream ss;
    ss << body.size();
    AddHeader("Content-Length", ss.str());
    m_body = body;
  }

private:
  std::map<std::string, std::string> m_headers;
  std::string m_body;
};

static void RequestProcess(HttpRequest &request, HttpResponse &reponse);

void RequestHandler(SocketUThread *st, int fd, void *data) {
  Socket *socket = (Socket *)data;
  while (1) {
    // step1. recv data
    int ret = st->Read(fd, (void *)socket->m_recvBuffer.UnusedData(),
                       socket->m_recvBuffer.UnusedLength());
    if (ret <= 0) {
      break;
    }
    // printf("request: %s\n", socket->m_recvBuffer.m_data.c_str() );

    // step2. parse request
    HttpRequest request;
    request.Parse(socket->m_recvBuffer.m_data);

    HttpResponse reponse;
    // step3. handle package
    RequestProcess(request, reponse);

    // step4. send out
    std::string resp = reponse.AsString();
    // socket->m_sendBuffer.m_data += resp;

    // step4. send data
    // int writen = st->Write(fd, (void*) socket->m_sendBuffer.Data(),
    // socket->m_sendBuffer.UsedLength() );
    // printf("resp: %s\n", resp.c_str() );
    int writen = st->Write(fd, (void *)resp.c_str(), resp.size());
    if (writen <= 0) {
      break;
    }
    socket->m_sendBuffer.m_data.clear();
    break;
  }

  delete socket;
  close(fd);
}
static void AddConnectionHandler(SocketUThread *st, int fd, void *data);
struct IoThread {
public:
  IoThread() {
    m_loop = new EventLoop();
    m_loopUt = new EventLoopUThread(m_loop);

    m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_eventFd < 0 && errno == EINVAL) {
      m_eventFd = eventfd(0, 0);
    }

    if (m_eventFd >= 0) {
      m_loopUt->SpawnWithFd(m_eventFd, AddConnectionHandler, this);
    }
  }

  void AddConnection(int fd) {
    uint64_t data = fd;
    int ret = write(m_eventFd, &data, sizeof(uint64_t));
  }

  void *Run() {
    printf("io thread loop on\n");
    m_loop->Dispath();
  }

public:
  EventLoop *m_loop;
  EventLoopUThread *m_loopUt;
  int m_eventFd;
  IoEvent m_addFdEvent;
};

static void AddConnectionHandler(SocketUThread *st, int fd, void *data) {
  IoThread *iothread = (IoThread *)data;
  uint64_t connFd = -1;
  while (1) {
    int ret = st->Read(iothread->m_eventFd, &connFd, sizeof(uint64_t));
    if (ret < 0) {
      printf("get error in event fd=%d\n", iothread->m_eventFd);

    } else if (ret > 0 && connFd >= 0) {
      printf("%lu thread get a conn=%d\n", pthread_self(), connFd);
      Socket *socket = new Socket(connFd);
      iothread->m_loopUt->SpawnWithFd(connFd, RequestHandler, socket);
    }
  }
}

typedef void *(*PthreadProc)(void *);
struct Server {
  Server(int ioThreadCnt) : m_ioThreadCnt(ioThreadCnt), m_robinCnt(0) {
    m_ioThreads = new IoThread[m_ioThreadCnt]();
    m_threads = new pthread_t[m_ioThreadCnt];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    for (int i = 0; i < m_ioThreadCnt; ++i) {
      pthread_create(&m_threads[i], &attr, (PthreadProc)&IoThread::Run,
                     &m_ioThreads[i]);
    }

    pthread_attr_destroy(&attr);
  }

  IoThread *m_ioThreads;
  int m_ioThreadCnt;
  uint64_t m_robinCnt;
  pthread_t *m_threads;
};

static void AcceptHandler(SocketUThread *st, int fd, void *data) {
  Server *server = (Server *)data;
  while (1) {
    int connFd = st->Accept(fd, NULL, NULL);
    if (connFd < 0) {
      printf("listen error!\n");
      close(fd);
      break;
    }
    int robin = (server->m_robinCnt) % server->m_ioThreadCnt;
    server->m_robinCnt += 1;

    server->m_ioThreads[robin].AddConnection(connFd);
  }
}

void RequestProcess(HttpRequest &request, HttpResponse &response) {
  std::stringstream ss;
  ss << "path:" << request.m_path << "\n";
  ss << "query params:\n";
  std::map<std::string, std::string>::iterator it = request.m_query.begin();
  for (; it != request.m_query.end(); ++it) {
    ss << it->first << "=" << it->second << "\n";
  }
  response.AddBody(ss.str());
}

int main(int argc, char const *argv[]) {
  EventLoop *loop = new EventLoop();
  EventLoopUThread *loopUt = new EventLoopUThread(loop);
  Server *server = new Server(8);

  int listenFd = GetListenFd();
  loopUt->SpawnWithFd(listenFd, AcceptHandler, server);

  printf("http server is on\n");
  loop->Dispath();

  for (int i = 0; i < server->m_ioThreadCnt; ++i) {
    pthread_join(server->m_threads[i], NULL);
  }

  return 0;
}
