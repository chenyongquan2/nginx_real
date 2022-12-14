//和网络 有关的函数放这里

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

//--------------------------------------------------------------------------
//构造函数
CSocekt::CSocekt()
{
    //配置相关
    m_workerProcessorConnMaxCnt = 1;      //epoll连接最大项数
    m_listenPortsCnt = 1;         //监听一个端口
    m_recycleConnAfterTime = 60; //等待这么些秒后才回收连接

    //epoll相关
    m_epollHandle = -1;          //epoll返回的句柄
    //m_pconnections = NULL;       //连接池【连接数组】先给空
    //m_pfree_connections = NULL;  //连接池中空闲的连接链 
    //m_pread_events = NULL;       //读事件数组给空
    //m_pwrite_events = NULL;      //写事件数组给空

    //一些和网络通讯有关的常用变量值，供后续频繁使用时提高效率
    m_pkgHeaderSize = sizeof(NgxPkgHeaderInfo);    //包头的sizeof值【占用的字节数】
    m_pkgMsgHeaderSize =  sizeof(NgxExtraMsgHeaderInfo);  //消息头的sizeof值【占用的字节数】

    //多线程相关
    //pthread_mutex_init(&m_recvMessageQueueMutex, NULL); //互斥量初始化    

    //各种队列相关
    m_sendMsgSize = 0;//发消息队列大小
    m_recycleConnListSize = 0;//待释放连接队列大小
    m_timerQueueMapSize = 0;//当前计时队列尺寸
    m_timerQueueFrontVal = 0;//当前计时队列头部的时间值
    m_iDiscardSendPkgCount = 0;//丢弃的发送数据包数量

    //在线用户相关
    m_onlineUserCount = 0;//在线用户数量统计，先给0  
    m_lastPrintTime = 0;//上次打印统计信息的时间，先给0
    return;	
}

//初始化函数【fork()子进程之前干这个事】
//成功返回true，失败返回false
bool CSocekt::Initialize()
{
    ReadConf();  //读配置项
    if(_NgxOpenListeningSockets() == false)  //打开监听端口    
        return false;  
    return true;
}

//子进程中才需要执行的初始化函数
bool CSocekt::InitForSubProc()
{
    //发消息互斥量初始化
    if(pthread_mutex_init(&m_sendMsgQueueMutex, NULL)  != 0)
    {        
        ngx_log_stderr(0,"CSocekt::InitForSubProc()中pthread_mutex_init(&m_sendMsgQueueMutex)失败.");
        return false;    
    }
    //连接相关互斥量初始化
    if(pthread_mutex_init(&m_connMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocekt::InitForSubProc()中pthread_mutex_init(&m_connMutex)失败.");
        return false;    
    }    
    //连接回收队列相关互斥量初始化
    if(pthread_mutex_init(&m_recycleConnQueueMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocekt::InitForSubProc()中pthread_mutex_init(&m_recycleConnQueueMutex)失败.");
        return false;    
    } 
    //和时间处理队列有关的互斥量初始化
    if(pthread_mutex_init(&m_timeQueueMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocekt::InitForSubProc()中pthread_mutex_init(&m_timeQueueMutex)失败.");
        return false;    
    }
   
    //初始化发消息相关信号量，信号量用于进程/线程 之间的同步，虽然 互斥量[pthread_mutex_lock]和 条件变量[pthread_cond_wait]都是线程之间的同步手段，但
    //这里用信号量实现 则 更容易理解，更容易简化问题，使用书写的代码短小且清晰；
    //第二个参数=0，表示信号量在线程之间共享，确实如此 ，如果非0，表示在进程之间共享
    //第三个参数=0，表示信号量的初始值，为0时，调用sem_wait()就会卡在那里卡着
    if(sem_init(&m_semEventSendQueue,0,0) == -1)
    {
        ngx_log_stderr(0,"CSocekt::InitForSubProc()中sem_init(&m_semEventSendQueue,0,0)失败.");
        return false;
    }

    int err = 0;
    {
        //1.创建专门发送数据给客户端的线程/处理发送消息队列的线程
        ThreadItemInSocket *pThreadItemInSocket = new ThreadItemInSocket(this);//专门用来发送数据的线程
        m_threadItemVec.push_back(pThreadItemInSocket);//创建 一个新线程对象 并入到容器中 
        //创建线程，错误不返回到errno，一般返回错误码
        err = pthread_create(&pThreadItemInSocket->m_threadHandle, NULL, ServerSendQueueThread, pThreadItemInSocket);
        if(err != 0)
        {
            ngx_log_stderr(0,"CSocekt::InitForSubProc()中pthread_create(ServerSendQueueThread)失败.");
            return false;
        }
    }
    
    {
        //2.创建专门处理延迟回收连接的线程
        ThreadItemInSocket *pThreadItemInSocket = new ThreadItemInSocket(this);//专门用来回收连接的线程
        m_threadItemVec.push_back(pThreadItemInSocket); 
        err = pthread_create(&pThreadItemInSocket->m_threadHandle, NULL, ServerRecyConnectionThread,pThreadItemInSocket);
        if(err != 0)
        {
            ngx_log_stderr(0,"CSocekt::InitForSubProc()中pthread_create(ServerRecyConnectionThread)失败.");
            return false;
        }
    }

    {
        //3.创建定时踢人的线程
        if(m_bKickConnWhenTimeOut == 1)  //是否开启踢人时钟
        {
            ThreadItemInSocket *pThreadItemInSocket = new ThreadItemInSocket(this);//专门用来处理到期不发心跳包的用户踢出的线程
            m_threadItemVec.push_back(pThreadItemInSocket); 
            err = pthread_create(&pThreadItemInSocket->m_threadHandle, NULL, ServerTimerQueueMonitorThread,pThreadItemInSocket);
            if(err != 0)
            {
                ngx_log_stderr(0,"CSocekt::InitForSubProc()中pthread_create(ServerTimerQueueMonitorThread)失败.");
                return false;
            }
        }
    }

    return true;
}

//--------------------------------------------------------------------------
//释放函数
CSocekt::~CSocekt()
{
    //释放必须的内存
    //(1)监听端口相关内存的释放--------
    std::vector<NgxListeningInfo*>::iterator pos;	
	for(pos = m_listenSocketVec.begin(); pos != m_listenSocketVec.end(); ++pos) //vector	
		delete (*pos); //一定要把指针指向的内存干掉，不然内存泄漏

	m_listenSocketVec.clear();    
    return;
}

//关闭退出函数[子进程中执行]
void CSocekt::ShutdownSubProc()
{
    //(1)把干活的线程停止掉，注意 系统应该尝试通过设置 g_processStopFlag = 1来 开始让整个项目停止
    //(2)用到信号量的，可能还需要调用一下sem_post

    //唤醒发送消息的线程，让其快点干完剩下的活 把剩余的数据都发给客户端后再优雅的退出。
    if(sem_post(&m_semEventSendQueue)==-1)  
    {
         ngx_log_stderr(0,"CSocekt::ShutdownSubProc()中sem_post(&m_semEventSendQueue)失败.");
    }

    std::vector<ThreadItemInSocket*>::iterator iter;
	for(iter = m_threadItemVec.begin(); iter != m_threadItemVec.end(); iter++)
    {
        pthread_join((*iter)->m_threadHandle, NULL); //等待一个线程终止
    }
    //(2)释放一下new出来的ThreadItem【线程池中的线程】    
	for(iter = m_threadItemVec.begin(); iter != m_threadItemVec.end(); iter++)
	{
		if(*iter)
			delete *iter;
	}
	m_threadItemVec.clear();

    //(3)队列相关
    _ClearMsgSendQueue();
    _ClearConnPool();
    _ClearAllFromTimerQueue();
    
    //(4)多线程相关    
    pthread_mutex_destroy(&m_connMutex);          //连接相关互斥量释放
    pthread_mutex_destroy(&m_sendMsgQueueMutex);    //发消息互斥量释放    
    pthread_mutex_destroy(&m_recycleConnQueueMutex);       //连接回收队列相关的互斥量释放
    pthread_mutex_destroy(&m_timeQueueMutex);           //时间处理队列相关的互斥量释放
    sem_destroy(&m_semEventSendQueue);                  //发消息相关线程信号量释放
}

//清理TCP发送消息队列
void CSocekt::_ClearMsgSendQueue()
{
	char * sTmpMempoint;
	CMemory *pMemoryInstance = CMemory::GetInstance();
	
	while(!m_sendMsgQueue.empty())
	{
		sTmpMempoint = m_sendMsgQueue.front();
		m_sendMsgQueue.pop_front(); 
		pMemoryInstance->FreeMemory(sTmpMempoint);
	}	
}

//专门用于读各种配置项
void CSocekt::ReadConf()
{
    CConfig *p_config = CConfig::GetInstance();
    //epoll连接的最大项数
    m_workerProcessorConnMaxCnt = p_config->GetIntDefault("worker_connections",m_workerProcessorConnMaxCnt);
    //取得要监听的端口数量
    m_listenPortsCnt = p_config->GetIntDefault("ListenPortCount",m_listenPortsCnt);  
    //等待这么些秒后才回收连接                  
    m_recycleConnAfterTime = p_config->GetIntDefault("Sock_RecyConnectionWaitTime",m_recycleConnAfterTime); 

    //是否开启踢人时钟，1：开启 0：不开启
    m_bKickConnWhenTimeOut = p_config->GetIntDefault("Sock_WaitTimeEnable",0);   
    //多少秒检测一次是否 心跳超时，只有当Sock_WaitTimeEnable = 1时，本项才有用	                             
	m_iWaitTime = p_config->GetIntDefault("Sock_MaxWaitTime",m_iWaitTime);    
	m_iWaitTime = (m_iWaitTime > 5) ? m_iWaitTime:5;//不建议低于5秒钟，因为无需太频繁  
    //当时间到达Sock_MaxWaitTime指定的时间时，直接把客户端踢出去，只有当Sock_WaitTimeEnable = 1时，本项才有用                                                     
    m_ifTimeOutKick = p_config->GetIntDefault("Sock_TimeOutKick", 0); 

    //Flood攻击检测是否开启,1：开启   0：不开启
    m_floodAkEnable = p_config->GetIntDefault("Sock_FloodAttackKickEnable",0);   
    //表示每次收到数据包的时间间隔是100(毫秒)                       
	m_floodTimeInterval = p_config->GetIntDefault("Sock_FloodTimeInterval",100); 
    //累积多少次踢出此人                           
	m_floodAtkMaxCnt = p_config->GetIntDefault("Sock_FloodKickCounter",10);                              

    return;
}

//监听端口【支持多个端口】，这里遵从nginx的函数命名
//在创建worker进程之前就要执行这个函数；
bool CSocekt::_NgxOpenListeningSockets()
{    
    char strinfo[100];//临时字符串 
    //服务器的地址结构体
    struct sockaddr_in serv_addr;            
    memset(&serv_addr,0,sizeof(serv_addr));//先初始化一下
    serv_addr.sin_family = AF_INET; //选择协议族为IPV4
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);//监听本地所有的IP地址；INADDR_ANY表示的是一个服务器上所有的网卡（服务器可能不止一个网卡）多个本地ip地址都进行绑定端口号，进行侦听。

    CConfig *p_config = CConfig::GetInstance();
    for(int i = 0; i < m_listenPortsCnt; i++) //要监听这么多个端口
    {        
        //参数1：AF_INET：使用ipv4协议，一般就这么写
        //参数2：SOCK_STREAM：使用TCP，表示可靠连接【相对还有一个UDP套接字，表示不可靠连接】
        //参数3：给0，固定用法，就这么记
        int isock = socket(AF_INET,SOCK_STREAM,0); //系统函数，成功返回非负描述符，出错返回-1
        if(isock == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中socket()失败,i=%d.",i);
            //其实这里直接退出，那如果以往有成功创建的socket呢？就没得到释放吧，当然走到这里表示程序不正常，应该整个退出，也没必要释放了 
            return false;
        }

        //setsockopt（）:设置一些套接字参数选项；
        //参数2：是表示级别，和参数3配套使用，也就是说，参数3如果确定了，参数2就确定了;
        //参数3：允许重用本地地址
        //设置 SO_REUSEADDR，目的第五章第三节讲解的非常清楚：主要是解决TIME_WAIT这个状态导致bind()失败的问题
        int reuseaddr = 1;  //1:打开对应的设置项
        if(setsockopt(isock,SOL_SOCKET, SO_REUSEADDR,(const void *) &reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中setsockopt(SO_REUSEADDR)失败,i=%d.",i);
            close(isock); //无需理会是否正常执行了                                                  
            return false;
        }

        //Todo:为处理惊群问题使用reuseport
        int reuseport = 1;
        if (setsockopt(isock, SOL_SOCKET, SO_REUSEPORT,(const void *) &reuseport, sizeof(int))== -1) //端口复用需要内核支持
        {
            //失败就失败吧，失败顶多是惊群，但程序依旧可以正常运行，所以仅仅提示一下即可
            ngx_log_stderr(errno,"CSocekt::Initialize()中setsockopt(SO_REUSEPORT)失败",i);
        }
        //else
        //{
        //    ngx_log_stderr(errno,"CSocekt::Initialize()中setsockopt(SO_REUSEPORT)成功");
        //}
        

        //设置该socket为非阻塞
        if(_SetNonBlocking(isock) == false)
        {                
            ngx_log_stderr(errno,"CSocekt::Initialize()中setnonblocking()失败,i=%d.",i);
            close(isock);
            return false;
        }

        //设置本服务器要监听的地址和端口，这样客户端才能连接到该地址和端口并发送数据        
        strinfo[0] = 0;
        sprintf(strinfo,"ListenPort%d",i);
        int iport = p_config->GetIntDefault(strinfo,10000);
        serv_addr.sin_port = htons((in_port_t)iport);   //in_port_t其实就是uint16_t

        //绑定服务器地址结构体
        if(bind(isock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中bind()失败,i=%d.",i);
            close(isock);
            return false;
        }
        
        //开始监听
        if(listen(isock,NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno,"CSocekt::Initialize()中listen()失败,i=%d.",i);
            close(isock);
            return false;
        }

        //可以，放到列表里来
        NgxListeningInfo* p_listensocketitem = new NgxListeningInfo; //千万不要写错，注意前边类型是指针，后边类型是一个结构体
        memset(p_listensocketitem,0,sizeof(NgxListeningInfo));      //注意后边用的是 ngx_listening_t而不是lpngx_listening_t
        p_listensocketitem->port = iport;                          //记录下所监听的端口号
        p_listensocketitem->fd   = isock;                          //套接字木柄保存下来   
        ngx_log_error_core(NGX_LOG_INFO,0,"监听%d端口成功!",iport); //显示一些信息到日志中
        m_listenSocketVec.push_back(p_listensocketitem);          //加入到队列中
    } 

    if(m_listenSocketVec.size() <= 0)  //不可能一个端口都不监听吧
        return false;
    return true;
}

//设置socket连接为非阻塞模式【这种函数的写法很固定】：非阻塞，概念在五章四节讲解的非常清楚【不断调用，不断调用这种：拷贝数据的时候是阻塞的】
bool CSocekt::_SetNonBlocking(int sockfd) 
{    
    //写法1:
    int nb=1; //0：清除，1：设置  
    if(ioctl(sockfd, FIONBIO, &nb) == -1) //FIONBIO：设置/清除非阻塞I/O标记：0：清除，1：设置
    {
        return false;
    }
    return true;

    //写法2：
    /* 
    //fcntl:file control【文件控制】相关函数，执行各种描述符控制操作
    //参数1：所要设置的描述符，这里是套接字【也是描述符的一种】
    int opts = fcntl(sockfd, F_GETFL);  //用F_GETFL先获取描述符的一些标志信息
    if(opts < 0) 
    {
        ngx_log_stderr(errno,"CSocekt::_SetNonBlocking()中fcntl(F_GETFL)失败.");
        return false;
    }
    opts |= O_NONBLOCK; //把非阻塞标记加到原来的标记上，标记这是个非阻塞套接字【如何关闭非阻塞呢？opts &= ~O_NONBLOCK,然后再F_SETFL一下即可】
    if(fcntl(sockfd, F_SETFL, opts) < 0) 
    {
        ngx_log_stderr(errno,"CSocekt::_SetNonBlocking()中fcntl(F_SETFL)失败.");
        return false;
    }
    return true;
    */
}

//Todo:这个方法暂时没用到。
void CSocekt::_NgxCloselisteningSockets()
{
    for(int i = 0; i < m_listenPortsCnt; i++)
    {  
        //ngx_log_stderr(0,"端口是%d,socketid是%d.",m_listenSocketVec[i]->port,m_listenSocketVec[i]->fd);
        close(m_listenSocketVec[i]->fd);
        ngx_log_error_core(NGX_LOG_INFO,0,"关闭监听端口%d!",m_listenSocketVec[i]->port); //显示一些信息到日志中
    }
    return;
}

/////////////发送消息队列的生产者!!!
//将一个待发送消息入到发消息队列中
void CSocekt::_PushMsgToSendQueue(char *pSendBufPos) 
{
    CMemory *pMemoryInstance = CMemory::GetInstance();
    CLock lock(&m_sendMsgQueueMutex); 
    //发送消息队列过大也可能给服务器带来风险
    if(m_sendMsgSize > 50000)
    {
        //发送队列过大，比如客户端恶意不接受数据，就会导致这个队列越来越大
        //那么可以考虑为了服务器安全，干掉一些数据的发送，虽然有可能导致客户端出现问题，但总比服务器不稳定要好很多
        m_iDiscardSendPkgCount++;
        pMemoryInstance->FreeMemory(pSendBufPos);
		return;
    }
    
    //总体数据并无风险，不会导致服务器崩溃，要看看个体数据，找一下恶意者了    
    NgxExtraMsgHeaderInfo* pMsgHeader = (NgxExtraMsgHeaderInfo*)pSendBufPos;
	NgxConnectionInfo* pConn = pMsgHeader->pConn;
    if(pConn->iSendCount > 400)
    {
        //该用户收消息太慢【或者干脆不收消息】，累积的该用户的发送队列中有的数据条目数过大，认为是恶意用户，直接切断
        ngx_log_stderr(0,"CSocekt::_PushMsgToSendQueue()中发现某用户%d积压了大量待发送数据包，切断与他的连接！",pConn->fd);      
        m_iDiscardSendPkgCount++;
        pMemoryInstance->FreeMemory(pSendBufPos);
        zdClosesocketProc(pConn); //直接关闭
		return;
    }

    ++pConn->iSendCount; //发送队列中有的数据条目数+1；
    m_sendMsgQueue.push_back(pSendBufPos);     
    ++m_sendMsgSize;   //原子操作

    //发送消息队列 来了一个可供发送的数据,唤醒阻塞在条件变量等待可供发送数据的线程
    if(sem_post(&m_semEventSendQueue)==-1)
    {
        ngx_log_stderr(0,"CSocekt::_PushMsgToSendQueue()中sem_post(&m_semEventSendQueue)失败.");      
    }
    return;
}

//主动关闭一个连接时的要做些善后的处理函数
//这个函数是可能被多线程调用的，但是即便被多线程调用，也没关系，不影响本服务器程序的稳定性和正确运行性
void CSocekt::zdClosesocketProc(NgxConnectionInfo* pConn)
{
    if(m_bKickConnWhenTimeOut == 1)
    {
        _DeleteConnFromTimerQueue(pConn); //从时间队列中把连接干掉
    }
    if(pConn->fd != -1)
    {   
        close(pConn->fd); //这个socket关闭，关闭后epoll就会被从红黑树中删除，所以这之后无法收到任何epoll事件
        pConn->fd = -1;
    }

    if(pConn->iConnWaitEpollOutCntsWhenNotSendAll > 0)  
        --pConn->iConnWaitEpollOutCntsWhenNotSendAll;   //归0

    _inRecycleConnQueue(pConn);
    return;
}

//测试是否flood攻击成立，成立则返回true，否则返回false
bool CSocekt::_TestFlood(NgxConnectionInfo* pConn)
{
    struct timeval sCurrTime;//当前时间结构
	uint64_t iCurrTime;   //当前时间（单位：毫秒）
	bool reco  = false;
	
	gettimeofday(&sCurrTime, NULL); //取得当前时间
    iCurrTime =  (sCurrTime.tv_sec * 1000 + sCurrTime.tv_usec / 1000);  //毫秒
	if((iCurrTime - pConn->FloodkickLastTime) < m_floodTimeInterval)   //两次收到包的时间 < 100毫秒
	{
        //发包太频繁记录
		pConn->FloodAttackCount++;
		pConn->FloodkickLastTime = iCurrTime;
	}
	else
	{
        //既然发布不这么频繁，则恢复计数值
		pConn->FloodAttackCount = 0;
		pConn->FloodkickLastTime = iCurrTime;
	}

    //ngx_log_stderr(0,"pConn->FloodAttackCount=%d,m_floodAtkMaxCnt=%d.",pConn->FloodAttackCount,m_floodAtkMaxCnt);

	if(pConn->FloodAttackCount >= m_floodAtkMaxCnt)
	{
		//可以踢此人的标志
		reco = true;
	}
	return reco;
}

//打印统计信息
void CSocekt::printTDInfo()
{
    //return;
    time_t currtime = time(NULL);
    if( (currtime - m_lastPrintTime) > 10)
    {
        //超过10秒我们打印一次
        int tmprmqc = g_threadpool.GetMsgRecvQueueCount(); //收消息队列

        m_lastPrintTime = currtime;
        int tmpoLUC = m_onlineUserCount;    //atomic做个中转，直接打印atomic类型报错；
        int tmpsmqc = m_sendMsgSize; //atomic做个中转，直接打印atomic类型报错；
        ngx_log_stderr(0,"------------------------------------begin--------------------------------------");
        ngx_log_stderr(0,"当前在线人数/总人数(%d/%d)。",tmpoLUC,m_workerProcessorConnMaxCnt);        
        ngx_log_stderr(0,"连接池中空闲连接/总连接/要释放的连接(%d/%d/%d)。",m_freeConnList.size(),m_allConnList.size(),m_recycleConnList.size());
        ngx_log_stderr(0,"当前时间队列大小(%d)。",m_timerQueueMap.size());        
        ngx_log_stderr(0,"当前收消息队列/发消息队列大小分别为(%d/%d)，丢弃的待发送数据包数量为%d。",tmprmqc,tmpsmqc,m_iDiscardSendPkgCount);        
        if( tmprmqc > 100000)
        {
            //接收队列过大，报一下，这个属于应该 引起警觉的，考虑限速等等手段
            ngx_log_stderr(0,"接收队列条目数量过大(%d)，要考虑限速或者增加处理线程数量了！！！！！！",tmprmqc);
        }
        ngx_log_stderr(0,"-------------------------------------end---------------------------------------");
    }
    return;
}

//本函数被NgxWorkerProcessInit()所调用
int CSocekt::NgxInitEpollAndConnPool()
{
    //(1)很多内核版本不处理epoll_create的参数，只要该参数>0即可
    //创建一个epoll对象，创建了一个红黑树，还创建了一个双向链表
    m_epollHandle = epoll_create(m_workerProcessorConnMaxCnt);   //直接以epoll连接的最大项数为参数，肯定是>0的； 
    if (m_epollHandle == -1) 
    {
        ngx_log_stderr(errno,"CSocekt::NgxInitEpollAndConnPool()中epoll_create()失败.");
        exit(2);
    }

    //(2)创建连接池【数组】、创建出来，这个东西后续用于处理所有客户端的连接
    _InitConnPool();
    {
        //之前写法是用一个数组 数组里面每一个元素都有一个next指针;现在改为了两个数组(一个存放所有conn 一个存放free conn)
        /*
        m_connection_n = m_workerProcessorConnMaxCnt;      //记录当前连接池中连接总数
        //连接池【数组，每个元素是一个对象】
        m_pconnections = new NgxConnectionInfo[m_connection_n]; //new不可以失败，不用判断结果，如果失败直接报异常更好一些
    
        int i = m_connection_n;                //连接池中连接数
        NgxConnectionInfo* next  = NULL;
        NgxConnectionInfo* c = m_pconnections; //连接池数组首地址
        do 
        {
            i--;   //注意i是数字的末尾，从最后遍历，i递减至数组首个元素

            //好从屁股往前来---------
            c[i].data = next;         //设置连接对象的next指针，注意第一次循环时next = NULL;
            c[i].fd = -1;             //初始化连接，无socket和该连接池中的连接【对象】绑定
            c[i].instance = 1;        //失效标志位设置为1【失效】，此句抄自官方nginx，这句到底有啥用，后续再研究
            c[i].iCurrsequence = 0;   //当前序号统一从0开始
            //----------------------

            next = &c[i]; //next指针前移
        } while (i); //循环直至i为0，即数组首地址
        m_pfree_connections = next;            //设置空闲连接链表头指针,因为现在next指向c[0]，注意现在整个链表都是空的
        m_freeConnCnt = m_connection_n;  //空闲连接链表长度，因为现在整个链表都是空的，这两个长度相等；
        */
    }
    
    //(3)为每个监听socket增加一个连接池中的连接【说白了就是让一个socket和一个内存绑定，以方便记录该sokcet相关的数据、状态等等】
	for(auto pos = m_listenSocketVec.begin(); pos != m_listenSocketVec.end(); ++pos)
    {
        //从连接池中获取一个空闲连接对象
        NgxConnectionInfo* pConn = _NgxGetFreeConn((*pos)->fd);//将监听socket的fd给丢进去。
        if (pConn == NULL)
        {
            ngx_log_stderr(errno,"CSocekt::NgxInitEpollAndConnPool()中ngx_get_connection()失败.");
            exit(2);
        }
        pConn->listening = (*pos); //连接对象和监听对象关联，方便通过连接对象找监听对象
        (*pos)->connection = pConn;//监听对象和连接对象关联，方便通过监听对象找连接对象

        //rev->accept = 1; //监听端口必须设置accept标志为1  ，这个是否有必要，再研究

        //设置监听socket对应连接的读事件为_NgxAcceptConn()，处理三次握手后建立的连接。
        pConn->rhandler = &CSocekt::_NgxAcceptConn;

        if(NgxEpollOperatorEvent(
            (*pos)->fd,         //socekt句柄
            EPOLL_CTL_ADD,      //事件类型，这里是增加
            EPOLLIN|EPOLLRDHUP, //标志，这里代表要增加的标志,EPOLLIN：可读，EPOLLRDHUP：TCP连接的远端关闭或者半关闭
            0,                  //对于事件类型为增加的，不需要这个参数
            pConn              //连接池中的连接 
        ) == -1) 
        {
            exit(2); //有问题，直接退出，日志 已经写过了
        }
    }
    return 1;
}

//对epoll事件的具体操作
//返回值：成功返回1，失败返回-1；
int CSocekt::NgxEpollOperatorEvent(
                        int                fd,               //句柄，一个socket
                        uint32_t           eventtype,        //事件类型，一般是EPOLL_CTL_ADD，EPOLL_CTL_MOD，EPOLL_CTL_DEL ，说白了就是操作epoll红黑树的节点(增加，修改，删除)
                        uint32_t           flag,             //标志，具体含义取决于eventtype
                        int                bcaction,         //补充动作，用于补充flag标记的不足  :  0：增加   1：去掉 2：完全覆盖 ,eventtype是EPOLL_CTL_MOD时这个参数就有用
                        NgxConnectionInfo* pConn             //pConn：一个指针【其实是一个连接】，EPOLL_CTL_ADD时增加到红黑树中去，将来epoll_wait时能取出来用
                        )
{
    struct epoll_event ev;    
    memset(&ev, 0, sizeof(ev));

    if(eventtype == EPOLL_CTL_ADD) //往红黑树中增加节点；
    {
        //红黑树从无到有增加节点
        //ev.data.ptr = (void *)pConn;
        ev.events = flag;      //既然是增加节点，则不管原来是啥标记
        pConn->events = flag;  //这个连接本身也记录这个标记
    }
    else if(eventtype == EPOLL_CTL_MOD)
    {
        //节点已经在红黑树中，修改节点的事件信息
        ev.events = pConn->events;  //先把标记恢复回来
        if(bcaction == 0)
        {
            //增加某个标记            
            ev.events |= flag;
        }
        else if(bcaction == 1)
        {
            //去掉某个标记
            ev.events &= ~flag;
        }
        else
        {
            //完全覆盖某个标记            
            ev.events = flag;      //完全覆盖            
        }
        pConn->events = ev.events; //记录该标记
    }
    else
    {
        //删除红黑树中节点，目前没这个需求【socket关闭这项会自动从红黑树移除】，所以将来再扩展
        return  1;  //先直接返回1表示成功
    } 

    //原来的理解中，绑定ptr这个事，只在EPOLL_CTL_ADD的时候做一次即可，但是发现EPOLL_CTL_MOD似乎会破坏掉.data.ptr，因此不管是EPOLL_CTL_ADD，还是EPOLL_CTL_MOD，都给进去
    //找了下内核源码SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd,		struct epoll_event __user *, event)，感觉真的会覆盖掉：
       //copy_from_user(&epds, event, sizeof(struct epoll_event)))，感觉这个内核处理这个事情太粗暴了
    ev.data.ptr = (void *)pConn;

    if(epoll_ctl(m_epollHandle,eventtype,fd,&ev) == -1)
    {
        ngx_log_stderr(errno,"CSocekt::NgxEpollOperatorEvent()中epoll_ctl(%d,%ud,%ud,%d)失败.",fd,eventtype,flag,bcaction);    
        return -1;
    }
    return 1;
}

//开始获取发生的事件消息
//参数unsigned int timer：epoll_wait()阻塞的时长，单位是毫秒；
//返回值，1：正常返回  ,0：有问题返回，一般不管是正常还是问题返回，都应该保持进程继续运行
//本函数被ngx_process_events_and_timers()调用，而ngx_process_events_and_timers()是在子进程的死循环中被反复调用
int CSocekt::NgxEpollProcessEvents(int timer) 
{   
    //等待事件，事件会返回到m_events里，最多返回NGX_MAX_EVENTS个事件【因为我只提供了这些内存】；
    //如果两次调用epoll_wait()的事件间隔比较长，则可能在epoll的双向链表中，积累了多个事件，所以调用epoll_wait，可能取到多个事件
    //阻塞timer这么长时间除非：a)阻塞时间到达 b)阻塞期间收到事件【比如新用户连入】会立刻返回c)调用时有事件也会立刻返回d)如果来个信号，比如你用kill -1 pid测试
    //如果timer为-1则一直阻塞，如果timer为0则立即返回，即便没有任何事件
    //返回值：有错误发生返回-1，错误在errno中，比如你发个信号过来，就返回-1，错误信息是(4: Interrupted system call)
    //       如果你等待的是一段时间，并且超时了，则返回0；
    //       如果返回>0则表示成功捕获到这么多个事件【返回值里】
    int readyEventsCnt = epoll_wait(m_epollHandle, m_eventsArr, NGX_MAX_EVENTS, timer);    
    if(readyEventsCnt == -1) //还没到时间!
    {
       
        //有错误发生，发送某个信号给本进程就可以导致这个条件成立，而且错误码根据观察是4；
        //#define EINTR  4，EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。
               //例如：在socket服务器端，设置了信号捕获机制，有子进程，当在父进程阻塞于慢系统调用时由父进程捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误(被中断的系统调用)。
        if(errno == EINTR) 
        {
            //信号所致，直接返回，一般认为这不是毛病，但还是打印下日志记录一下，因为一般也不会人为给worker进程发送消息
            ngx_log_error_core(NGX_LOG_INFO,errno,"CSocekt::NgxEpollProcessEvents()中epoll_wait()失败!"); 
            return 1;  //正常返回
        }
        else
        {
            //这被认为应该是有问题，记录日志
            ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocekt::NgxEpollProcessEvents()中epoll_wait()失败!"); 
            return 0;  //非正常返回 
        }
    }

    if(readyEventsCnt == 0) //超时，但没事件来
    {
        if(timer != -1)
            return 1;//要求epoll_wait阻塞一定的时间而不是一直阻塞，这属于阻塞到时间了，则正常返回
        
        //无限等待【所以不存在超时】，但却没返回任何事件，这应该不正常有问题        
        return 0; //非正常返回 
    }

    //会惊群，一个telnet上来，4个worker进程都会被惊动，都执行下边这个
    //ngx_log_stderr(0,"惊群测试:events=%d,进程id=%d",events,g_curPid); 
    //ngx_log_stderr(0,"----------------------------------------"); 

    //走到这里，就是属于有事件收到了
    for(int i = 0; i < readyEventsCnt; ++i)    //遍历本次epoll_wait返回的所有事件，注意events才是返回的实际事件数量
    {
        NgxConnectionInfo* pConn = (NgxConnectionInfo*)(m_eventsArr[i].data.ptr);//ngx_epoll_add_event()给进去的，这里能取出来

        //能走到这里，我们认为这些事件都没过期，就正常开始处理
        uint32_t eventsType = m_eventsArr[i].events;//取出事件类型
        
        /*
        if(eventsType & (EPOLLERR|EPOLLHUP)) //例如对方close掉套接字，这里会感应到【换句话说：如果发生了错误或者客户端断连】
        {
            //这加上读写标记，方便后续代码处理，至于怎么处理，后续再说，这里也是参照nginx官方代码引入的这段代码；
            //官方说法：if the error events were returned, add EPOLLIN and EPOLLOUT，to handle the events at least in one active handler
            //我认为官方也是经过反复思考才加上着东西的，先放这里放着吧； 
            eventsType |= EPOLLIN|EPOLLOUT;   //EPOLLIN：表示对应的链接上有数据可以读出（TCP链接的远端主动关闭连接，也相当于可读事件，因为本服务器小处理发送来的FIN包）
                                           //EPOLLOUT：表示对应的连接上可以写入数据发送【写准备好】            
        } */

        if(eventsType & EPOLLIN) //如果是读事件
        {
            //1)一个客户端新连入，这个会成立，
            //2)已连接发送数据来，这个也成立；
            //ngx_log_stderr(errno,"数据来了来了来了 ~~~~~~~~~~~~~.");
            (this->* (pConn->rhandler) )(pConn);    //注意括号的运用来正确设置优先级，防止编译出错；
            //如果新连接进入，这里执行的应该是CSocekt::_NgxAcceptConn(c)】            
            //如果是已经连入，发送数据到这里，则这里执行的应该是 CSocekt::_NgxReadRequestHandler()                        
        }
        
        if(eventsType & EPOLLOUT) //如果是写事件【对方关闭连接也触发这个，再研究。。。。。。】，注意上边的 if(revents & (EPOLLERR|EPOLLHUP))  revents |= EPOLLIN|EPOLLOUT; 读写标记都给加上了
        {
            //ngx_log_stderr(errno,"22222222222222222222.");
            if(eventsType & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) //客户端关闭，如果服务器端挂着一个写通知事件，则这里个条件是可能成立的
            {
                //EPOLLERR：对应的连接发生错误                     8     = 1000 
                //EPOLLHUP：对应的连接被挂起                       16    = 0001 0000
                //EPOLLRDHUP：表示TCP连接的远端关闭或者半关闭连接   8192   = 0010  0000   0000   0000
                //我想打印一下日志看一下是否会出现这种情况
                //8221 = ‭0010 0000 0001 1101‬  ：包括 EPOLLRDHUP ，EPOLLHUP， EPOLLERR
                //ngx_log_stderr(errno,"CSocekt::NgxEpollProcessEvents()中revents&EPOLLOUT成立并且revents & (EPOLLERR|EPOLLHUP|EPOLLRDHUP)成立,event=%ud。",revents); 

                //我们只有投递了 写事件，但对端断开时，程序流程才走到这里，投递了写事件意味着 iConnWaitEpollOutCntsWhenNotSendAll标记肯定被+1了，这里我们减回
                --pConn->iConnWaitEpollOutCntsWhenNotSendAll;                 
            }
            else
            {
                //因为我们只有在write()没完全发送整个数据包时(只发送了一部分)才会去把可写事件给添加进去epoll!
                //则这里执行的应该是 CSocekt::_NgxWriteRequestHandler()
                (this->* (pConn->whandler) )(pConn);   
            }            
        }
    }   
    return 1;
}

//===========处理发送消息队列的消费者!!!
//处理发送消息队列的线程
void* CSocekt::ServerSendQueueThread(void* threadData)//静态函数
{   
    CMemory *pMemoryInstance = CMemory::GetInstance();
    ThreadItemInSocket *pThread = static_cast<ThreadItemInSocket*>(threadData);
    CSocekt *pSocketObj = pThread->m_ptrSocket;
    while(g_processStopFlag == 0) //不退出
    {
        //如果信号量值>0，则 -1(减1) 并走下去，否则卡这里卡着【为了让信号量值+1，可以在其他线程调用sem_post达到，实际上在CSocekt::_PushMsgToSendQueue()调用sem_post就达到了让这里sem_wait走下去的目的】
        //******如果被某个信号中断，sem_wait也可能过早的返回，错误为EINTR；
        //整个程序退出之前，也要sem_post()一下，确保如果本线程卡在sem_wait()，也能走下去从而让本线程成功返回
        if(sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        {
            if(errno != EINTR)//这个我就不算个错误了【当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。】
                ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中sem_wait(&pSocketObj->m_semEventSendQueue)失败.");            
        }

        if(g_processStopFlag != 0)
            break;

        if(pSocketObj->m_sendMsgSize > 0) //原子的 
        {
            int err = pthread_mutex_lock(&pSocketObj->m_sendMsgQueueMutex); //因为我们要操作发送消息对列m_MsgSendQueue，所以这里要临界            
            if(err != 0) 
                ngx_log_stderr(err,"CSocekt::ServerSendQueueThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);

            auto pos = pSocketObj->m_sendMsgQueue.begin();
			auto posEnd = pSocketObj->m_sendMsgQueue.end();

            while(pos != posEnd)
            {
                char *pMsgBuf = (*pos);//拿到的每个消息都是 消息头+包头+包体【但要注意，我们是不发送消息头给客户端的】
                NgxExtraMsgHeaderInfo* pMsgHeader = (NgxExtraMsgHeaderInfo*)pMsgBuf;//指向消息头
                NgxPkgHeaderInfo* pPkgHeader = (NgxPkgHeaderInfo*)(pMsgBuf+pSocketObj->m_pkgMsgHeaderSize);//指向包头
                NgxConnectionInfo* pConnInfo = pMsgHeader->pConn;

                //而且这里有没必要针对本连接来用m_connectionMutex临界 ,只要下面条件成立，肯定是客户端连接已断，要发送的数据肯定不需要发送了
                if(pConnInfo->iCurrsequence != pMsgHeader->iCurrsequence) 
                {
                    //包过期，因为如果这个连接被回收，比如在ngx_close_connection(),_inRecycleConnQueue()中都会自增iCurrsequence
                    auto  delPos = pos;
                    pos++;
                    pSocketObj->m_sendMsgQueue.erase(delPos);
                    --pSocketObj->m_sendMsgSize; //发送消息队列容量少1		
                    pMemoryInstance->FreeMemory(pMsgBuf);	
                    continue;
                }

                if(pConnInfo->iConnWaitEpollOutCntsWhenNotSendAll > 0)//当前连接的已经出现发送不出去了(例如可能客户端的接收缓冲区已经满了!)
                {
                    //这个连接靠系统驱动来发送消息，所以这里不能再发送
                    pos++;
                    continue;
                }

                --pConnInfo->iSendCount;//发送队列中有的数据条目数-1；
    
                //走到这里，可以发送消息，一些必须的信息记录，要发送的东西也要从发送队列里干掉
                auto delPos = pos;
				pos++;
                pSocketObj->m_sendMsgQueue.erase(delPos);
                --pSocketObj->m_sendMsgSize;      //发送消息队列容量少1	

                pConnInfo->ptrNewMemForSend = pMsgBuf;//发送后释放用的，因为这段内存是new出来的
                pConnInfo->pSendBufPos = (char *)pPkgHeader;//要发送的数据的缓冲区指针，因为发送数据不一定全部都能发送出去，我们要记录数据发送到了哪里，需要知道下次数据从哪里开始发送
                unsigned short itmp = ntohs(pPkgHeader->pkgLen); //包头+包体 长度 ，打包时用了htons【本机序转网络序】，所以这里为了得到该数值，用了个ntohs【网络序转本机序】；
                pConnInfo->lessSendSize = itmp; //要发送多少数据，因为发送数据不一定全部都能发送出去，我们需要知道剩余有多少数据还没发送
                                
                //这里是重点，我们采用 epoll水平触发的策略，能走到这里的，都应该是还没有投递 写事件 到epoll中
                    //epoll水平触发发送数据的改进方案：
	                //开始不把socket写事件通知加入到epoll,当我需要写数据的时候，直接调用write/send发送数据；
	                //如果返回了EAGIN【发送缓冲区满了，需要等待可写事件才能继续往缓冲区里写数据】，此时，我再把写事件通知加入到epoll，
	                //此时，就变成了在epoll驱动下写数据，全部数据发送完毕后，再把写事件通知从epoll中干掉；
	                //优点：数据不多的时候，可以避免epoll的写事件的增加/删除，提高了程序的执行效率；                         
                //(1)直接调用write或者send发送数据
                //ngx_log_stderr(errno,"即将发送数据%ud。",pConnInfo->lessSendSize);

                ssize_t sendsize = pSocketObj->_SendToClient(pConnInfo, pConnInfo->pSendBufPos, pConnInfo->lessSendSize); //注意参数
                if (sendsize > 0)
                {                    
                    if(sendsize == pConnInfo->lessSendSize)
                    {
                        //全部发送成功了
                        pMemoryInstance->FreeMemory(pConnInfo->ptrNewMemForSend);
                        pConnInfo->ptrNewMemForSend = NULL;
                        pConnInfo->iConnWaitEpollOutCntsWhenNotSendAll = 0;  //这行其实可以没有，因此此时此刻这东西就是=0的                        
                        //ngx_log_stderr(0,"CSocekt::ServerSendQueueThread()中数据发送完毕，很好。"); //做个提示吧，商用时可以干掉
                    }
                    else//没有全部发送完毕(EAGAIN)，数据只发出去了一部分，因为该socket的发送缓冲区满了
                    {         
                        //发送到了哪里，剩余多少，记录下来，方便下次sendproc()时使用
                        pConnInfo->pSendBufPos = pConnInfo->pSendBufPos + sendsize;
				        pConnInfo->lessSendSize = pConnInfo->lessSendSize - sendsize;	
                        
                        //标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送
                        ++pConnInfo->iConnWaitEpollOutCntsWhenNotSendAll;//【原子+1，且不可写成p_Conn->iConnWaitEpollOutCntsWhenNotSendAll = pConn->iConnWaitEpollOutCntsWhenNotSendAll +1 ，这种写法不是原子+1】

                        //后续等待该连接出现可写通知 epoll驱动调用ngx_write_request_handler()函数发送数据
                        if(pSocketObj->NgxEpollOperatorEvent(
                            pConnInfo->fd,      //socket句柄
                            EPOLL_CTL_MOD,      //事件类型，这里是增加【因为我们准备增加个写通知】
                            EPOLLOUT,           //标志，这里代表要增加的标志,EPOLLOUT：可写【可写的时候通知我】
                            0,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                            pConnInfo           //连接池中的连接
                        ) == -1)
                        {
                            ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()NgxEpollOperatorEvent()失败.");
                        }

                        //ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中数据没发送完毕【发送缓冲区满】，整个要发送%d，实际发送了%d。",pConn->lessSendSize,sendsize);

                    }
                    continue;               
                } 
                else if(sendsize == -1)
                {
                    //发送缓冲区已经满了【一个字节都没发出去，说明发送 缓冲区当前正好是满的】
                    ++pConnInfo->iConnWaitEpollOutCntsWhenNotSendAll; //标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送
                    //投递此事件后，我们将依靠epoll驱动调用ngx_write_request_handler()函数发送数据
                    if(pSocketObj->NgxEpollOperatorEvent(
                                pConnInfo->fd,         //socket句柄
                                EPOLL_CTL_MOD,      //事件类型，这里是增加【因为我们准备增加个写通知】
                                EPOLLOUT,           //标志，这里代表要增加的标志,EPOLLOUT：可写【可写的时候通知我】
                                0,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                                pConnInfo              //连接池中的连接
                                ) == -1)
                    {
                        //有这情况发生？这可比较麻烦，不过先do nothing
                        ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中ngx_epoll_add_event()_2失败.");
                    }
                    continue;
                }
                else if(sendsize == 0)
                {
                    //能走到这里，应该是有点问题的
                    //发送0个字节，首先因为我发送的内容不是0个字节的；
                    //然后如果发送 缓冲区满则返回的应该是-1，而错误码应该是EAGAIN，所以我综合认为，这种情况我就把这个发送的包丢弃了【按对端关闭了socket处理】
                    //这个打印下日志，我还真想观察观察是否真有这种现象发生
                    //ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中sendproc()居然返回0？"); //如果对方关闭连接出现send=0，那么这个日志可能会常出现，商用时就 应该干掉
                    pMemoryInstance->FreeMemory(pConnInfo->ptrNewMemForSend);  //释放内存
                    pConnInfo->ptrNewMemForSend = NULL;
                    pConnInfo->iConnWaitEpollOutCntsWhenNotSendAll = 0;  //这行其实可以没有，因此此时此刻这东西就是=0的    
                    continue;
                }
                else
                {
                    //能走到这里的，应该就是返回值-2了，一般就认为对端断开了，等待recv()来做断开socket以及回收资源
                    pMemoryInstance->FreeMemory(pConnInfo->ptrNewMemForSend);  //释放内存
                    pConnInfo->ptrNewMemForSend = NULL;
                    pConnInfo->iConnWaitEpollOutCntsWhenNotSendAll = 0;  //这行其实可以没有，因此此时此刻这东西就是=0的  
                    continue;
                }
            }

            err = pthread_mutex_unlock(&pSocketObj->m_sendMsgQueueMutex); 
            if(err != 0)  
                ngx_log_stderr(err,"CSocekt::ServerSendQueueThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);
            
        }
    }
    
    return (void*)0;
}
