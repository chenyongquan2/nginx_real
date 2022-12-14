
//和网络 中 连接/连接池 有关的函数放这里

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

//---------------------------------------------------------------
//连接池成员函数
NgxConnectionInfo::NgxConnectionInfo()
{		
    iCurrsequence = 0;    
    pthread_mutex_init(&logicPorcMutex, NULL); 
}

NgxConnectionInfo::~NgxConnectionInfo()
{
    pthread_mutex_destroy(&logicPorcMutex);
}

//分配出去一个连接的时候初始化一些内容,原来内容放在 _NgxGetFreeConn()里，现在放在这里
void NgxConnectionInfo::initConnectionInfo()
{
    ++iCurrsequence;

    fd  = -1;                                         //开始先给-1
    curStat = _PKG_HD_INIT;                           //收包状态处于 初始状态，准备接收数据包头【状态机】
    pRecvBufPos = dataHeadInfo;                          //收包我要先收到这里来，因为我要先收包头，所以收数据的buff直接就是dataHeadInfo
    lessRecvSize = sizeof(NgxPkgHeaderInfo);               //这里指定收数据的长度，这里先要求收包头这么长字节的数据
    
    ptrNewMemForRecv   = NULL;                         //既然没new内存，那自然指向的内存地址先给NULL
    iConnWaitEpollOutCntsWhenNotSendAll   = 0;                            //原子的
    ptrNewMemForSend   = NULL;                         //发送数据头指针记录
    events            = 0;                            //epoll事件先给0 
    lastPingTime      = time(NULL);                   //上次ping的时间

    FloodkickLastTime = 0;                            //Flood攻击上次收到包的时间
	FloodAttackCount  = 0;	                          //Flood攻击在该时间内收到包的次数统计
    iSendCount        = 0;                            //发送队列中有的数据条目数，若client只发不收，则可能造成此数过大，依据此数做出踢出处理 
}

//回收回来一个连接的时候做一些事
void NgxConnectionInfo::resetAndFreeConnectionInfo()
{
    ++iCurrsequence;   
    if(ptrNewMemForRecv != NULL)//我们曾经给这个连接分配过接收数据的内存，则要释放内存
    {        
        CMemory::GetInstance()->FreeMemory(ptrNewMemForRecv);
        ptrNewMemForRecv = NULL;        
    }
    if(ptrNewMemForSend != NULL) //如果发送数据的缓冲区里有内容，则要释放内存
    {
        CMemory::GetInstance()->FreeMemory(ptrNewMemForSend);
        ptrNewMemForSend = NULL;
    }
    iConnWaitEpollOutCntsWhenNotSendAll = 0;                              //设置不设置感觉都行         
}

//初始化连接池
void CSocekt::_InitConnPool()
{
    CMemory *pMemoryInstance = CMemory::GetInstance();   
    int NgxConnectionInfoSz = sizeof(NgxConnectionInfo);    
    for(int i = 0; i < m_workerProcessorConnMaxCnt; ++i) //先创建这么多个连接，后续不够再增加
    {
        NgxConnectionInfo* pConn = (NgxConnectionInfo*)pMemoryInstance->AllocMemory(NgxConnectionInfoSz,true);
    
        //手工调用构造函数，因为AllocMemory里无法调用构造函数
        pConn = new(pConn) NgxConnectionInfo(); 	//定位new，释放则显式调用p_Conn->~NgxConnectionInfo();	
        pConn->initConnectionInfo();
        m_allConnList.push_back(pConn);//所有链接【不管是否空闲】都放在这个list
        m_freeConnList.push_back(pConn);//空闲连接会放在这个list
    }

    m_freeConnCnt = m_totalConnCnt = m_allConnList.size(); //开始这两个列表一样大
    return;
}

//最终回收连接池，释放内存
void CSocekt::_ClearConnPool()
{
    NgxConnectionInfo* pConn;
	CMemory *pMemoryInstance = CMemory::GetInstance();
	
	while(!m_allConnList.empty())
	{
		pConn = m_allConnList.front();
		m_allConnList.pop_front(); 
        pConn->~NgxConnectionInfo();     //手工调用析构函数
		pMemoryInstance->FreeMemory(pConn);
	}
}

//从连接池中获取一个空闲连接【当一个客户端连接TCP进入，我希望把这个连接和我的 连接池中的一个连接【对象】绑到一起，后续 我可以通过这个连接，把这个对象拿到，因为对象里边可以记录各种信息】
NgxConnectionInfo* CSocekt::_NgxGetFreeConn(int isock)
{
    //因为可能有其他线程要访问m_freeconnectionList，m_connectionList【比如可能有专门的释放线程要释放/或者主线程要释放】之类的
    CLock lock(&m_connMutex);  
    if(!m_freeConnList.empty())
    {
        NgxConnectionInfo* pConn = m_freeConnList.front(); 
        m_freeConnList.pop_front();
        pConn->initConnectionInfo();
        --m_freeConnCnt; 
        pConn->fd = isock;//将conn与socket进行绑定
        return pConn;
    }

    //走到这里，表示没空闲的连接了，那就考虑重新创建一个连接
    CMemory *pMemoryInstance = CMemory::GetInstance();
    NgxConnectionInfo* pConn = (NgxConnectionInfo*)pMemoryInstance->AllocMemory(sizeof(NgxConnectionInfo),true);
    pConn = new(pConn) NgxConnectionInfo();
    pConn->initConnectionInfo();
    m_allConnList.push_back(pConn);
    ++m_totalConnCnt;             
    pConn->fd = isock;
    return pConn;

    {
    //因为我们要采用延迟释放的手段来释放连接，因此这种 instance就没啥用，这种手段用来处理立即释放才有用。
    /*
    NgxConnectionInfo*  c = m_pfree_connections; //空闲连接链表头
    if(c == NULL)
    {
        //系统应该控制连接数量，防止空闲连接被耗尽，能走到这里，都不正常
        ngx_log_stderr(0,"CSocekt::_NgxGetFreeConn()中空闲链表为空,这不应该!");
        return NULL;
    }
    m_pfree_connections = c->next;                       //指向连接池中下一个未用的节点
    m_freeConnCnt--;                               //空闲连接少1

    //(1)注意这里的操作,先把c指向的对象中有用的东西搞出来保存成变量，因为这些数据可能有用
    uintptr_t  instance = c->instance;            //常规c->instance在刚构造连接池时这里是1【失效】
    uint64_t   iCurrsequence = c->iCurrsequence;  //序号也暂存，后续用于恢复
    //....其他内容再增加
    //(2)把以往有用的数据搞出来后，清空并给适当值
    memset(c,0,sizeof(NgxConnectionInfo));                //注意，类型不要用成lpngx_connection_t，否则就出错了
    c->fd      = isock;                                  //套接字要保存起来，这东西具有唯一性    
    c->curStat = _PKG_HD_INIT;                           //收包状态处于 初始状态，准备接收数据包头【状态机】

    c->pRecvBufPos = c->dataHeadInfo;                       //收包我要先收到这里来，因为我要先收包头，所以收数据的buff直接就是dataHeadInfo
    c->lessRecvSize = sizeof(NgxPkgHeaderInfo);               //这里指定收数据的长度，这里先要求收包头这么长字节的数据

    c->ptrNewMemForRecv = NULL;                            //既然没new内存，那自然指向的内存地址先给NULL
    c->iConnWaitEpollOutCntsWhenNotSendAll = 0;                              //原子的
    c->ptrNewMemForSend = NULL;                          //发送数据头指针记录
    //....其他内容再增加

    //(3)这个值有用，所以在上边(1)中被保留，没有被清空，这里又把这个值赋回来
    c->instance = !instance;                            //抄自官方nginx，到底有啥用，以后再说【分配内存时候，连接池里每个连接对象这个变量给的值都为1，所以这里取反应该是0【有效】；】
    c->iCurrsequence=iCurrsequence;++c->iCurrsequence;  //每次取用该值都增加1

    //wev->write = 1;  这个标记有没有 意义加，后续再研究
    return c;    
    */
   }
}

//归还参数pConn所代表的连接到到连接池中，注意参数类型是lpngx_connection_t
void CSocekt::_NgxFreeConnToFreeList(NgxConnectionInfo* pConn) 
{
    //因为有线程可能要动连接池中连接，所以在合理互斥也是必要的
    CLock lock(&m_connMutex);  

    //首先明确一点，连接，所有连接全部都在m_connectionList里；
    pConn->resetAndFreeConnectionInfo();

    //扔到空闲连接列表里
    m_freeConnList.push_back(pConn);
    ++m_freeConnCnt;//空闲连接数+1

    /*
    if(c->ptrNewMemForRecv != NULL)
    {
        //我们曾经给这个连接分配过内存，则要释放内存        
        CMemory::GetInstance()->FreeMemory(c->ptrNewMemForRecv);
        c->ptrNewMemForRecv = NULL;
        //c->ifnewrecvMem = false;  //这行有用？
    }
    if(c->ptrNewMemForSend != NULL) //如果发送数据的缓冲区里有内容，则要释放内存
    {
        CMemory::GetInstance()->FreeMemory(c->ptrNewMemForSend);
        c->ptrNewMemForSend = NULL;
    }

    c->next = m_pfree_connections;                       //回收的节点指向原来串起来的空闲链的链头

    //节点本身也要干一些事
    ++c->iCurrsequence;                                  //回收后，该值就增加1,以用于判断某些网络事件是否过期【一被释放就立即+1也是有必要的】
    c->iConnWaitEpollOutCntsWhenNotSendAll = 0;                              //设置不设置感觉都行     

    m_pfree_connections = c;                             //修改 原来的链头使链头指向新节点
    ++m_freeConnCnt;                               //空闲连接多1    
    */
    return;
}


//将要回收的连接放到一个队列中来，专门的线程会来延迟回收该链接(给点时间让该链接把手头的活给干完。)
//有些连接，我们不希望马上释放，要隔一段时间后再释放以确保服务器的稳定，所以，我们把这种隔一段时间才释放的连接先放到一个队列中来
void CSocekt::_inRecycleConnQueue(NgxConnectionInfo* pConn)
{
    //ngx_log_stderr(0,"CSocekt::_inRecycleConnQueue()执行，连接入到回收队列中.");
    CLock lock(&m_recycleConnQueueMutex); //针对连接回收列表的互斥量，因为线程ServerRecyConnectionThread()也有要用到这个回收列表；
    for(auto pos = m_recycleConnList.begin(); pos != m_recycleConnList.end(); ++pos)
	{
		if((*pos) == pConn)		
			return;	//防止被多次扔到回收队列中来
	}

    pConn->inRecyTime = time(NULL);//记录回收时间
    ++pConn->iCurrsequence;
    m_recycleConnList.push_back(pConn); //等待ServerRecyConnectionThread线程自会处理 
    ++m_recycleConnListSize; //待释放连接队列大小+1
    --m_onlineUserCount; //连入用户数量-1
    return;
}

//处理延迟回收链接的线程
void* CSocekt::ServerRecyConnectionThread(void* threadData)
{
    ThreadItemInSocket *pThread = static_cast<ThreadItemInSocket*>(threadData);
    CSocekt *pSocketObj = pThread->m_ptrSocket;
    
    int err;
    std::list<NgxConnectionInfo*>::iterator pos,posend;
    NgxConnectionInfo* pConn;
    
    while(1)
    {
        //为简化问题，我们直接每次休息200毫秒
        usleep(200 * 1000);  //单位是微妙,又因为1毫秒=1000微妙，所以 200 *1000 = 200毫秒
        if(pSocketObj->m_recycleConnListSize > 0)//原子的
        {
            time_t curTime = time(NULL);
            err = pthread_mutex_lock(&pSocketObj->m_recycleConnQueueMutex);  
            if(err != 0) 
                ngx_log_stderr(err,"CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);
lblRRTD:
            pos    = pSocketObj->m_recycleConnList.begin();
			posend = pSocketObj->m_recycleConnList.end();
            for(; pos != posend; ++pos)
            {
                pConn = (*pos);
                if((pConn->inRecyTime + pSocketObj->m_recycleConnAfterTime) > curTime  && g_processStopFlag == 0 )//如果不是要整个系统退出，你可以continue，否则就得要强制释放
                    continue; //没到释放的时间
                  
                //到释放的时间了: 
                //这将来可能还要做一些是否能释放的判断[在我们写完发送数据代码之后吧]，先预留位置

                //我认为，凡是到释放时间的，iConnWaitEpollOutCntsWhenNotSendAll都应该为0；这里我们加点日志判断下
                //if(pConn->iConnWaitEpollOutCntsWhenNotSendAll != 0)
                if(pConn->iConnWaitEpollOutCntsWhenNotSendAll > 0)
                {
                    //这确实不应该，打印个日志吧；
                    ngx_log_stderr(0,"CSocekt::ServerRecyConnectionThread()中到释放时间却发现p_Conn.iConnWaitEpollOutCntsWhenNotSendAll!=0，这个不该发生");
                    //其他先暂时啥也不敢，路程继续往下走，继续去释放吧。
                }

                //从待回收链接队列中把某一个满足延迟回收的连接给移除。
                --pSocketObj->m_recycleConnListSize;//待释放连接队列大小-1
                pSocketObj->m_recycleConnList.erase(pos);//迭代器已经失效，但pos所指内容在p_Conn里保存着呢

                //ngx_log_stderr(0,"CSocekt::ServerRecyConnectionThread()执行，连接%d被归还.",pConn->fd);

                pSocketObj->_NgxFreeConnToFreeList(pConn);//归还参数pConn所代表的连接到到连接池中
                goto lblRRTD; 
            } 
            err = pthread_mutex_unlock(&pSocketObj->m_recycleConnQueueMutex); 
            if(err != 0)  
                ngx_log_stderr(err,"CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);
        } 

        if(g_processStopFlag == 1) //要退出整个程序，那么肯定要先退出这个循环
        {
            if(pSocketObj->m_recycleConnListSize > 0)
            {
                //因为要退出，所以就得硬释放了【不管到没到时间，不管有没有其他不 允许释放的需求，都得硬释放】
                err = pthread_mutex_lock(&pSocketObj->m_recycleConnQueueMutex);  
                if(err != 0) 
                    ngx_log_stderr(err,"CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock2()失败，返回的错误码为%d!",err);

        lblRRTD2:
                pos = pSocketObj->m_recycleConnList.begin();
			    posend = pSocketObj->m_recycleConnList.end();
                for(; pos != posend; ++pos)
                {
                    pConn = (*pos);

                    //从待回收链接队列中把某一个满足延迟回收的连接给移除。
                    --pSocketObj->m_recycleConnListSize;        //待释放连接队列大小-1
                    pSocketObj->m_recycleConnList.erase(pos);   //迭代器已经失效，但pos所指内容在p_Conn里保存着呢
                    pSocketObj->_NgxFreeConnToFreeList(pConn);	   //归还参数pConn所代表的连接到到连接池中
                    goto lblRRTD2; 
                } 
                err = pthread_mutex_unlock(&pSocketObj->m_recycleConnQueueMutex); 
                if(err != 0)  ngx_log_stderr(err,"CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock2()失败，返回的错误码为%d!",err);
            }
            break; //整个程序要退出了，所以break;
        }
    } 
    
    return (void*)0;
}

//用户连入，我们accept4()时，得到的socket在处理中产生失败，则资源用这个函数释放
//当连接分配出去 但是还么用来处理各种逻辑来收发数据，则可以直接进行关闭，而无需延迟关闭。
//大前提:因为其上还没有数据收发，谈不到业务逻辑因此无需延迟
void CSocekt::_NgxFreeConnAndCloseConnFd(NgxConnectionInfo* pConn)
{    
    //pConn->fd = -1; //官方nginx这么写，这么写有意义；    不要这个东西，回收时不要轻易东连接里边的内容
    _NgxFreeConnToFreeList(pConn); 
    if(pConn->fd != -1)
    {
        close(pConn->fd);
        pConn->fd = -1;
    }    
    return;
}
