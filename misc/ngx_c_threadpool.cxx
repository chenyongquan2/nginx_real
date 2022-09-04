//和 线程池 有关的函数放这里
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_threadpool.h"
#include "ngx_c_memory.h"
#include "ngx_macro.h"

pthread_mutex_t CThreadPool::m_pthreadMutex = PTHREAD_MUTEX_INITIALIZER;  //#define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t) -1)
pthread_cond_t CThreadPool::m_pthreadCond = PTHREAD_COND_INITIALIZER;     //#define PTHREAD_COND_INITIALIZER ((pthread_cond_t) -1)
bool CThreadPool::m_bShutdown = false;     

CThreadPool::CThreadPool()
{
    m_iRunningThreadNum = 0;
    m_lastReportAllThreadBusyTime = 0;//上次报告线程不够用了的时间；
    //m_iPrintInfoTime = 0;    //上次打印参考信息的时间；
    m_recvMsgQueueSize = 0;
}

CThreadPool::~CThreadPool()
{    
    //资源释放在StopAll()里统一进行，就不在这里进行了
    //接收消息队列中内容释放
    clearMsgRecvQueue();
}

//清理接收消息队列
void CThreadPool::clearMsgRecvQueue()
{
	CMemory *pMemInstance = CMemory::GetInstance();
	//尾声阶段，需要互斥？该退的都退出了，该停止的都停止了，应该不需要退出了
	while(!m_msgRecvQueue.empty())
	{
		char* pMsg= m_msgRecvQueue.front();		
		m_msgRecvQueue.pop_front(); 
		pMemInstance->FreeMemory(pMsg);
	}	
}

//创建线程池中的线程，要手工调用，不在构造函数里调用了
bool CThreadPool::Create(int threadNum)
{    
    m_iThreadNum = threadNum;  
    for(int i = 0; i < m_iThreadNum; ++i)
    {
        ThreadItem *pThreadItem = new ThreadItem(this);
        m_threadItemVec.push_back(pThreadItem);//创建一个新线程对象并入到容器中

        //创建线程，错误不返回到errno，一般返回错误码    
        int err = pthread_create(&pThreadItem->m_threadHandle, NULL, ThreadFunc, pThreadItem);
        if(err != 0)
        {
            ngx_log_stderr(err,"CThreadPool::Create()创建线程%d失败，返回的错误码为%d!",i,err);
            return false;
        }
        else
        {
            //创建线程成功
            //ngx_log_stderr(0,"CThreadPool::Create()创建线程%d成功,线程id=%d",pNew->m_threadHandle);
        }        
    } 

    //我们必须保证每个线程都启动并运行到线程执行函数的ThreadFunc()里面的pthread_cond_wait()，让所有线程都先在阻塞等待条件变量m_pthreadCond
lblfor:
    for(auto iter = m_threadItemVec.begin(); iter != m_threadItemVec.end(); iter++)
    {
        if( (*iter)->m_bRunning == false) //这个条件保证所有线程完全启动起来，以保证整个线程池中的线程正常工作；
        {
            //这说明有没有启动完全的线程
            usleep(100 * 1000);  //单位是微妙,又因为1毫秒=1000微妙，所以 100 *1000 = 100毫秒
            goto lblfor;
        }
    }
    return true;
}

/////////////////////////////////////////消费者!!!
//新线程的线程入口回调函数，注意不能为普通成员函数(静态成员可以)  
void* CThreadPool::ThreadFunc(void* threadData)//当用pthread_create()创建线程后，这个ThreadFunc()函数都会被立即执行；
{
    //这个是静态成员函数，是不存在this指针的,所以得通过ThreadItem拿到线程池的指针
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CThreadPool *pThreadPoolObj = pThread->m_ptrThreadPool;
    CMemory *pMemoryInstance = CMemory::GetInstance();	    
    pthread_t tid = pthread_self(); //获取线程自身id，以方便调试打印信息等    

    while(true)
    {
        //线程用pthread_mutex_lock()函数去锁定指定的mutex变量，若该mutex已经被另外一个线程锁定了，该调用将会阻塞线程直到mutex被解锁。  
        int err = pthread_mutex_lock(&m_pthreadMutex);  
        if(err != 0) 
            ngx_log_stderr(err,"CThreadPool::ThreadFunc()中pthread_mutex_lock()失败，返回的错误码为%d!",err);
        /*
        //以下这行程序写法技巧十分重要，必须要用while这种写法，
        //因为：pthread_cond_wait()是个值得注意的函数，调用一次pthread_cond_signal()可能会唤醒多个【惊群】【官方描述是 至少一个/pthread_cond_signal 在多处理器上可能同时唤醒多个线程】
        //老师也在《c++入门到精通 c++ 98/11/14/17》里第六章第十三节谈过虚假唤醒，实际上是一个意思；
        //老师也在《c++入门到精通 c++ 98/11/14/17》里第六章第八节谈过条件变量、wait()、notify_one()、notify_all()，其实跟这里的pthread_cond_wait、pthread_cond_signal、pthread_cond_broadcast非常类似
        //pthread_cond_wait()函数，如果只有一条消息 唤醒了两个线程干活，那么其中有一个线程拿不到消息，那如果不用while写，就会出问题，所以被惊醒后必须再次用while拿消息，拿到才走下来；
        //while( (jobbuf = g_socket.outMsgRecvQueue()) == NULL && m_bShutdown == false)
        */
        while ((pThreadPoolObj->m_msgRecvQueue.size() == 0) && m_bShutdown == false)
        {
            //客户端发送过来的消息队列为空，
            if(pThread->m_bRunning == false)         
            {
                //标记为true了才允许调用StopAll()：测试中发现如果Create()和StopAll()紧挨着调用，就会导致线程混乱，所以每个线程必须执行到这里，才认为是启动成功了；
                pThread->m_bRunning = true; 
            }   
               
            //ngx_log_stderr(0,"执行了pthread_cond_wait-------------begin");
            //刚开始执行pthread_cond_wait()的时候，会卡在这里，而且m_pthreadMutex会被释放掉；
            //如果这个pthread_cond_wait被唤醒【被唤醒后程序执行流程往下走的前提是拿到了锁--官方：pthread_cond_wait()返回时，互斥量再次被锁住】
            pthread_cond_wait(&m_pthreadCond, &m_pthreadMutex); //整个服务器程序刚初始化的时候，所有线程必然是卡在这里等待的；
            //消费者在这这里等待生产者通知其(通过条件变量)
            //ngx_log_stderr(0,"执行了pthread_cond_wait-------------end");
        }

        //若标记为中止，则表示 后面准备放到消息队列供消费者线程消费的线程 不去开启新的任务。而是静静的等待干完手头上剩余
        if(m_bShutdown)
        {   
            pthread_mutex_unlock(&m_pthreadMutex); //解锁互斥量
            break;                     
        }

        //走到这里，可以取得消息进行处理了【消息队列中必然有消息】,注意，目前还是互斥着呢
        char *pCmdBuf = pThreadPoolObj->m_msgRecvQueue.front(); 
        pThreadPoolObj->m_msgRecvQueue.pop_front();
        --pThreadPoolObj->m_recvMsgQueueSize;
               
        //可以解锁互斥量了
        err = pthread_mutex_unlock(&m_pthreadMutex); 
        if(err != 0) 
            ngx_log_stderr(err,"CThreadPool::ThreadFunc()中pthread_mutex_unlock()失败，返回的错误码为%d!",err);
        
        //有消息可以处理
        ++pThreadPoolObj->m_iRunningThreadNum;//原子+1【记录正在干活的线程数量增加1】，这比互斥量要快很多
        g_socket.ExecCmd(pCmdBuf);

        //ngx_log_stderr(0,"执行开始---begin,tid=%ui!",tid);
        //sleep(5); //临时测试代码
        //ngx_log_stderr(0,"执行结束---end,tid=%ui!",tid);

        pMemoryInstance->FreeMemory(pCmdBuf);              //释放消息内存 
        --pThreadPoolObj->m_iRunningThreadNum;     //原子-1【记录正在干活的线程数量减少1】

    } 

    //能走出来表示整个程序要结束啊，怎么判断所有线程都结束？
    return (void*)0;
}

//停止所有线程【等待结束线程池中所有线程，该函数返回后，应该是所有线程池中线程都结束了】
void CThreadPool::StopAll() 
{
    //(1)已经调用过，就不要重复调用了
    if(m_bShutdown == true)
        return;
    
    //标记为中止，使得后面准备放到消息队列供消费者线程消费的线程 不去开启新的任务。而是静静的等待干完手头上剩余的活。
    m_bShutdown = true;

    //(2)唤醒阻塞等待在该条件变量的所有生产者线程,让这些线程赶紧干完。
    int err = pthread_cond_broadcast(&m_pthreadCond); 
    if(err != 0)
    {
        //这肯定是有问题，要打印紧急日志
        ngx_log_stderr(err,"CThreadPool::StopAll()中pthread_cond_broadcast()失败，返回的错误码为%d!",err);
        return;
    }

    //(3)等待所有生产者线程,让这些线程赶紧干完 
	for(auto iter = m_threadItemVec.begin(); iter != m_threadItemVec.end(); iter++)
        pthread_join((*iter)->m_threadHandle, NULL);

    //流程走到这里，那么所有的线程池中的线程肯定都返回了；
    pthread_mutex_destroy(&m_pthreadMutex);
    pthread_cond_destroy(&m_pthreadCond);    

    //(4)释放一下new出来的ThreadItem【线程池中的线程】    
	for(auto iter = m_threadItemVec.begin(); iter != m_threadItemVec.end(); iter++)
	{
		if(*iter)
			delete *iter;
	}
	m_threadItemVec.clear();

    ngx_log_stderr(0,"CThreadPool::StopAll()成功返回，线程池中线程全部正常结束!");
    return;    
}

/////////////////////////////////////////生产者!!!
//收到一个完整消息后，入消息队列，并触发线程池中线程来处理该消息
void CThreadPool::RecvMsgToQueueAndSignal(char *buf)
{
    int err = pthread_mutex_lock(&m_pthreadMutex);     
    if(err != 0)
        ngx_log_stderr(err,"CThreadPool::RecvMsgToQueueAndSignal()pthread_mutex_lock()失败，返回的错误码为%d!",err);
        
    m_msgRecvQueue.push_back(buf);
    ++m_recvMsgQueueSize; 

    //取消互斥
    err = pthread_mutex_unlock(&m_pthreadMutex);   
    if(err != 0)
        ngx_log_stderr(err,"CThreadPool::RecvMsgToQueueAndSignal()pthread_mutex_unlock()失败，返回的错误码为%d!",err);

    //可以激发一个线程来干活了(唤醒消费者!!!)
    Call();                                  
    return;
}

//激发一个线程来干活了(唤醒消费者!!!)
void CThreadPool::Call()
{
    //ngx_log_stderr(0,"m_pthreadCondbegin--------------=%ui!",m_pthreadCond);  //数字5，此数字不靠谱
    //for(int i = 0; i <= 100; i++)
    //{
    int err = pthread_cond_signal(&m_pthreadCond); //唤醒一个等待该条件的线程，也就是可以唤醒卡在pthread_cond_wait()的线程
    if(err != 0 )
        ngx_log_stderr(err,"CThreadPool::Call()中pthread_cond_signal()失败，返回的错误码为%d!",err);
    
    //}
    //唤醒完100次，试试打印下m_pthreadCond值;
    //ngx_log_stderr(0,"m_pthreadCondend--------------=%ui!",m_pthreadCond);  //数字1

    //如果当前的工作线程全部都忙，则要报警
    //bool ifallthreadbusy = false;
    if(m_iThreadNum == m_iRunningThreadNum)
    {        
        //线程不够用了
        //ifallthreadbusy = true;
        time_t currtime = time(NULL);
        if(currtime - m_lastReportAllThreadBusyTime > 10) //两次报告之间的间隔必须超过10秒，不然如果一直出现当前工作线程全忙，但频繁报告日志也够烦的
        {
            m_lastReportAllThreadBusyTime = currtime;  //更新时间
            //写日志，通知这种紧急情况给用户，用户要考虑增加线程池中线程数量了
            ngx_log_stderr(0,"CThreadPool::Call()中发现线程池中当前空闲线程数量为0，要考虑扩容线程池了!");
        }
    }
    return;
/*
    //-------------------------------------------------------如下内容都是一些测试代码；
    //唤醒丢失？--------------------------------------------------------------------------
    //(2)整个工程中，只在一个线程（主线程）中调用了Call，所以不存在多个线程调用Call的情形。
    if(ifallthreadbusy == false)
    {
        //有空闲线程  ，有没有可能我这里调用   pthread_cond_signal()，但因为某个时刻线程曾经全忙过，导致本次调用 pthread_cond_signal()并没有激发某个线程的pthread_cond_wait()执行呢？
           //我认为这种可能性不排除，这叫 唤醒丢失。如果真出现这种问题，我们如何弥补？
        if(irmqc > 5) //我随便来个数字比如给个5吧
        {
            //如果有空闲线程，并且 接收消息队列中超过5条信息没有被处理，则我总感觉可能真的是 唤醒丢失
            //唤醒如果真丢失，我是否考虑这里多唤醒一次？以尝试逐渐补偿回丢失的唤醒？此法是否可行，我尚不可知，我打印一条日志【其实后来仔细相同：唤醒如果真丢失，也无所谓，因为ThreadFunc()会一直处理直到整个消息队列为空】
            ngx_log_stderr(0,"CThreadPool::Call()中感觉有唤醒丢失发生，irmqc = %d!",irmqc);

            int err = pthread_cond_signal(&m_pthreadCond); //唤醒一个等待该条件的线程，也就是可以唤醒卡在pthread_cond_wait()的线程
            if(err != 0 )
            {
                //这是有问题啊，要打印日志啊
                ngx_log_stderr(err,"CThreadPool::Call()中pthread_cond_signal 2()失败，返回的错误码为%d!",err);
            }
        }
    }  //end if

    //(3)准备打印一些参考信息【10秒打印一次】,当然是有触发本函数的情况下才行
    m_iCurrTime = time(NULL);
    if(m_iCurrTime - m_iPrintInfoTime > 10)
    {
        m_iPrintInfoTime = m_iCurrTime;
        int irunn = m_iRunningThreadNum;
        ngx_log_stderr(0,"信息：当前消息队列中的消息数为%d,整个线程池中线程数量为%d,正在运行的线程数量为 = %d!",irmqc,m_iThreadNum,irunn); //正常消息，三个数字为 1，X，0
    }
    */
    
}

//唤醒丢失问题，sem_t sem_write;
//参考信号量解决方案：https://blog.csdn.net/yusiguyuan/article/details/20215591  linux多线程编程--信号量和条件变量 唤醒丢失事件
