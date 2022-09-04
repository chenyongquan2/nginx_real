
#ifndef __NGX_THREADPOOL_H__
#define __NGX_THREADPOOL_H__

//Todo:重要学习线程池的思路写法
class CThreadPool
{
public:
    CThreadPool();               
    ~CThreadPool();                           

    bool Create(int threadNum); 
    void StopAll();

    void RecvMsgToQueueAndSignal(char *buf);        //收到一个完整消息后，入消息队列，并触发线程池中线程来处理该消息
    void Call();                                    //来任务了，调一个线程池中的线程下来干活  
    int  GetMsgRecvQueueCount(){return m_recvMsgQueueSize;} //获取接收消息队列大小

private:
    //新线程的线程入口回调函数，注意不能为普通成员函数(静态成员可以)  
    static void* ThreadFunc(void *threadData); 
private:
    void clearMsgRecvQueue();

private:
    //定义一个 线程池中的 线程 的结构，以后可能做一些统计之类的 功能扩展，所以引入这么个结构来 代表线程 感觉更方便一些；    
    class ThreadItem   
    {
    public:
        ThreadItem(CThreadPool *pCThreadPool)
            :m_ptrThreadPool(pCThreadPool)
            ,m_bRunning(false)
        {

        }                             
        ~ThreadItem(){}      
    public:
        pthread_t m_threadHandle;//线程句柄
        CThreadPool *m_ptrThreadPool; //记录线程池的指针	
        bool m_bRunning; //标记是否正式启动起来，启动起来后，才允许调用StopAll()来释放
    };

private:
    std::vector<ThreadItem *> m_threadItemVec;//线程数组
    int m_iThreadNum;//要创建的线程数量
    std::atomic<int> m_iRunningThreadNum; //线程数, 运行中的线程数，原子操作

    static pthread_mutex_t m_pthreadMutex;//线程同步互斥量/也叫线程同步锁
    static pthread_cond_t m_pthreadCond;//线程同步条件变量
    static bool m_bShutdown;//线程退出标志，false不退出，true退出
    
    time_t m_lastReportAllThreadBusyTime;//上次发生线程不够用【紧急事件】的时间,防止日志报的太频繁
    //time_t  m_iPrintInfoTime;//打印信息的一个间隔时间，我准备10秒打印出一些信息供参考和调试
    //time_t  m_iCurrTime;//当前时间

    //负责接收消息的消息队列
    std::list<char *> m_msgRecvQueue;//接收数据的消息队列 
	int m_recvMsgQueueSize;//收消息队列大小
};

#endif
