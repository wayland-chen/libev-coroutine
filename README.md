# libev-coroutine
C++重写了libev, 并此基础上用协程提供类似gevent的同步编程接口

1. 用C++写重了libev基本特性 (io,signal,timeout,stat,child,async) , 提供与libev类似的接口
2. 对swapcontext封装，实现协程基础接口
3. 在1、2的基础上，实现了类似greenlet的编程接口，支持同步的编程方式并获取异步的效率
4. 在 3的基础上，以类似greenlet的同步编程方式实现了One loop per thread
5. 在4基础上实现http server的demo: 主线程accept并通过eventfd把 conn_fd 分配给各个io线程，各个io线程接入http请求并发送回应。
