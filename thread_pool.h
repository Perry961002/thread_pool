//
// Created by perry on 2020/11/21.
//

#ifndef UNTITLED1_THREAD_POOL_H
#define UNTITLED1_THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <future>

template <typename T>
class SafeQueue {
public:
    SafeQueue() = default;

    ~SafeQueue() = default;

    SafeQueue(const SafeQueue&) = delete;
    SafeQueue(SafeQueue &&) = delete;
    SafeQueue& operator=(const SafeQueue&) = delete;
    SafeQueue& operator=(SafeQueue&&) = delete;

    //队列是否为空
    const bool empty() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    const int size() {
        std::unique_lock<std::mutex> lock(m_mutex); //互斥信号变量加锁
        return m_queue.size();
    }

    //队列添加元素
    void push(T& t) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.emplace(t);
    }

    //队列取出元素
    bool pop(T& t) {
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_queue.empty()) {
            return false;
        }
        t = std::move(m_queue.front()); //取出队首元素，返回队首元素值，并进行右值引用

        m_queue.pop(); //弹出入队的第一个元素

        return true;
    }

private:
    std::queue<T> m_queue; //利用模板函数构造队列
    std::mutex m_mutex;//访问互斥信号量

};

class ThreadPool {
public:
    // 生成固定大小的线程池
    ThreadPool(int nFixedThreadPoolSize) : m_bStop(false), m_bSelfRegulatingModel(false),
        m_nWorkThread(0) {
        if (nFixedThreadPoolSize < 1) nFixedThreadPoolSize = 1;
        if (nFixedThreadPoolSize > ThreadPool::MachineThreadNum) nFixedThreadPoolSize = ThreadPool::MachineThreadNum;
        m_vecThreadPool = std::vector<std::thread>(nFixedThreadPoolSize);
        for (int i = 0; i < nFixedThreadPoolSize; ++i) {
            m_vecThreadPool.emplace(m_vecThreadPool.begin() + i, std::thread(WorkThreadProc(i, this)));
        }
    }

    ThreadPool(const std::chrono::milliseconds &time) : m_bStop(false), m_bSelfRegulatingModel(true),
        m_durMaxIdleTime(time), m_nWorkThread(0) {
        int nSize = 2 * ThreadPool::MachineThreadNum;
        m_vecThreadPool = std::vector<std::thread>(nSize);
        for (int i = 0; i < ThreadPool::MachineThreadNum; ++i) {
            m_vecThreadPool.emplace(m_vecThreadPool.begin() + i, std::thread(WorkThreadProc(i, this)));
        }
        for (int i = ThreadPool::MachineThreadNum; i < nSize; ++i) {
            m_queThreads.push(i);
        }
    }

    ThreadPool(const ThreadPool &) = delete;

    ThreadPool(ThreadPool &&) = delete;

    ThreadPool& operator=(const ThreadPool &) = delete;

    ThreadPool& operator=(ThreadPool &&) = delete;

    ~ThreadPool() {
        m_bStop = true;
        m_CondVar.notify_all();  // 通知所以阻塞的线程来清空任务队列并退出
        for (auto &th : m_vecThreadPool) {
            if (th.joinable()) th.join();
        }
    }

    // 向线程池提交一个任务
    template<typename F, typename...Args>
    auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        // 连接函数和参数定义
        // forward被用来保持参数的右值属性不变
        std::function<decltype(f(args...))()> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

        // 封装获取任务对象，方便另外一个线程查看结果
        auto ptrPackagedTask = std::make_shared<std::packaged_task<decltype(f(args...))()> >(func);

        // 得到函数对象
        std::function<void()> task = [ptrPackagedTask]() {
            (*ptrPackagedTask)();
        };

        // 压入任务队列
        m_queTasks.push(task);

        // 唤醒一个等待中的线程
        m_CondVar.notify_one();

        // 返回先前注册的任务的异步对象
        return ptrPackagedTask->get_future();
    }
public:
    const static int MachineThreadNum;  // 机器线程数
private:
    bool m_bStop;  // 停止标志
    bool m_bSelfRegulatingModel;  // 是否使用自调节模式
    std::atomic_int m_nWorkThread;  // 工作线程个数
    std::mutex m_ControlMtx;  // 互斥锁
    std::condition_variable m_CondVar;  // 条件变量
    std::vector<std::thread> m_vecThreadPool;  // 线程池
    SafeQueue< std::function<void()>> m_queTasks;  // 任务队列
    std::queue<int> m_queThreads;  // 空闲线程队列
    std::chrono::milliseconds m_durMaxIdleTime;  // 最长等待时间
private:
    // 工作线程生成类
    class WorkThreadProc {
    public:
        WorkThreadProc(const int ID, ThreadPool *ptrPool) : m_nID(ID), m_bIsWork(true), m_ptrPool(ptrPool) {
            if (m_ptrPool != nullptr)
             ++(m_ptrPool->m_nWorkThread);
        }

        // 重载访问运算符
        void operator() () {
            if (m_ptrPool == nullptr) return;

            if (m_ptrPool->m_bSelfRegulatingModel == false) {
                WorkFixedSize();
            }
            else {
                WorkSelfRegulating();
            }
            --(m_ptrPool->m_nWorkThread);
        }

        void WorkFixedSize() {
            std::function<void()> task;  // 基础函数对象，用于取任务并执行
            bool bGetTask = false;

            while (!m_ptrPool->m_bStop && m_bIsWork) {
                {
                    // 阻塞当前线程，等待被唤醒
                    std::unique_lock<std::mutex> lock(m_ptrPool->m_ControlMtx);
                    // 在线程池停止工作或者任务队列不空时确认被唤醒
                    m_ptrPool->m_CondVar.wait(lock, [this]() {
                        return m_ptrPool->m_bStop || !m_ptrPool->m_queTasks.empty();
                    });
                    // 线程池停止工作并且没有任务可取时工作线程可以结束
                    if (m_ptrPool->m_bStop && m_ptrPool->m_queTasks.empty()) m_bIsWork = false;
                    else bGetTask = m_ptrPool->m_queTasks.pop(task);
                }
                // 任务取出成功，执行它
                if (bGetTask) {
                    task();
                    bGetTask = false;
                }
            }
        }

        void WorkSelfRegulating() {
            std::function<void()> task;  // 基础函数对象，用于取任务并执行
            bool bGetTask = false;

            while (!m_ptrPool->m_bStop && m_bIsWork) {
                {
                    // 阻塞当前线程，等待被唤醒
                    std::unique_lock<std::mutex> lock(m_ptrPool->m_ControlMtx);
                    // 在线程池停止工作或者任务队列不空时确认被唤醒
                    bool bWaitRes = m_ptrPool->m_CondVar.wait_for(lock, m_ptrPool->m_durMaxIdleTime, [this]() {
                        return m_ptrPool->m_bStop || !m_ptrPool->m_queTasks.empty();
                    });
                    // 正常被唤醒
                    if (bWaitRes) {
                        // 线程池停止工作并且没有任务可取时工作线程可以结束
                        if (m_ptrPool->m_bStop && m_ptrPool->m_queTasks.empty()) m_bIsWork = false;
                        else {
                            bGetTask = m_ptrPool->m_queTasks.pop(task);
                            // 如果还有很多任务没有完成，开始扩容
                            while (!m_ptrPool->m_queThreads.empty() &&
                                   m_ptrPool->m_queTasks.size() > 3 * m_ptrPool->m_nWorkThread) {
                                //std::cout << "Threads: " << m_ptrPool->m_nWorkThread << ", Tasks: " << m_ptrPool->m_queTasks.size() << std::endl;
                                int nThID = m_ptrPool->m_queThreads.front();
                                m_ptrPool->m_queThreads.pop();
                                m_ptrPool->m_vecThreadPool.emplace(m_ptrPool->m_vecThreadPool.begin() + nThID, std::thread(WorkThreadProc(nThID, m_ptrPool)));
                            }
                        }
                    }
                        // 等待超时
                    else {
                        // 工作线程个数比任务数还多，但还需要一定的线程
                        if (m_ptrPool->m_nWorkThread > m_ptrPool->m_queTasks.size() &&
                            m_ptrPool->m_nWorkThread > ThreadPool::MachineThreadNum / 2) {
                            //std::cout << "Threads: " << m_ptrPool->m_nWorkThread << ", Tasks: " << m_ptrPool->m_queTasks.size() << std::endl;
                            m_ptrPool->m_queThreads.push(m_nID);
                            m_bIsWork = false;
                        }
                    }
                }
                // 任务取出成功，执行它
                if (bGetTask) {
                    task();
                    bGetTask = false;
                }
            }
        }
    private:
        int m_nID; //线程ID
        bool m_bIsWork;
        ThreadPool *m_ptrPool;  //注册进来的线程池对象指针
    };
};

const int ThreadPool::MachineThreadNum = std::thread::hardware_concurrency();

#endif //UNTITLED1_THREAD_POOL_H
