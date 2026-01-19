#include "Thread.h"
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>


//不同于EventLoopThread(那个是IO线程的线程池),这是处理耗时业务用的线程池
//或者叫计算线程的线程池,它本身是可以直接提供给上层的服务器使用的muduo
//网络库中的其他模块组件并没有用到它.

//它仅仅是一个生产者消费者问题,使用阻塞队列即可完成需求.因而实现难度也远远
//低于EventLoopThreadPool,后者不仅要实现one loop per thread,而且唤醒线
//程使用了eventfd,更是要把Channel的回调函数和任务队列里的回调函数分开处理.
class ThreadPool
:NonCopy
{
public:
    using Task=std::function<void()>;

public:
    explicit ThreadPool(size_t queMaxSize,
        std::string_view name="ThreadPool");
    ~ThreadPool();
    void start(size_t threadNum);
    void stop();
    void addTask(Task cb);

private:
    void threadFunc();

private:
    std::string name_;
    bool running_;

    //队列大小超过这个数就判定满了
    size_t maxQueSize_;

    //线程数组
    std::vector<std::unique_ptr<Thread>> threadVec_;

    //任务队列/阻塞队列,需要用互斥锁保护
    std::queue<Task> que_;
    std::mutex mtx_;

    //设计两个条件变量是比较好的编程习惯
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
};