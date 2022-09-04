
#ifndef __NGX_SOCKET_H__
#define __NGX_SOCKET_H__

#include "ngx_comm.h"


//一些宏定义放在这里-----------------------------------------------------------
#define NGX_LISTEN_BACKLOG 511 //已完成连接队列，nginx给511，我们也先按照这个来：不懂这个数字的同学参考第五章第四节
#define NGX_MAX_EVENTS 512	   // epoll_wait一次最多接收这么多个事件，nginx中缺省是512，我们这里固定给成512就行，没太大必要修改

struct NgxConnectionInfo;
class CSocekt;

//一些专用结构定义放在这里，暂时不考虑放ngx_global.h里了
struct NgxListeningInfo //和监听端口有关的结构
{
	int port;//监听的端口号
	int fd;//套接字句柄socket
	NgxConnectionInfo *connection; //连接池中的一个连接，注意这是个指针
};

typedef void (CSocekt::*NgxEventDealFuncPtr)(NgxConnectionInfo *c); //定义成员函数指针

//(1)该结构表示一个TCP连接【客户端主动发起的、Nginx服务器被动接受的TCP连接】
struct NgxConnectionInfo
{
public:
	NgxConnectionInfo();
	virtual ~NgxConnectionInfo();
	void initConnectionInfo();		   //分配出去的时候初始化一些内容
	void resetAndFreeConnectionInfo(); //回收回来的时候做一些事情

public:
	int fd;//套接字句柄socket
	NgxListeningInfo *listening; //如果这个链接被分配给了一个监听套接字，那么这个里边就指向监听套接字对应的那个lpngx_listening_t的内存首地址

	uint64_t iCurrsequence;//我引入的一个序号，每次分配出去时+1，此法也有可能在一定程度上检测错包废包，具体怎么用，用到了再说
	struct sockaddr s_sockaddr;//保存对方地址信息用的

	NgxEventDealFuncPtr rhandler; //读事件的相关处理方法
	NgxEventDealFuncPtr whandler; //写事件的相关处理方法

	//和epoll事件有关
	uint32_t events; //和epoll事件有关

	//和收包有关
	unsigned char curStat;			   //当前收包的状态
	char dataHeadInfo[_DATA_BUFSIZE_]; //用于保存收到的数据的包头信息
	char *pRecvBufPos; //接收数据的缓冲区的头指针，对收到不全的包非常有用，看具体应用的代码
	unsigned int lessRecvSize;///剩余要收到多少数据
	char *ptrNewMemForRecv; //new出来的用于收包的内存首地址，释放用的

	pthread_mutex_t logicPorcMutex; //逻辑处理相关的互斥量

	//和发包有关
	std::atomic<int> iConnWaitEpollOutCntsWhenNotSendAll; //发送消息，如果发送缓冲区满了，则需要通过epoll事件来驱动消息的继续发送，所以如果发送缓冲区满，则用这个变量标记
	char *ptrNewMemForSend;//发送完成后释放用的，整个数据的头指针，其实是 消息头 + 包头 + 包体
	char *pSendBufPos;	//发送数据的缓冲区的头指针，开始 其实是包头+包体
	unsigned int lessSendSize;//剩余要发送多少数据

	//和回收有关
	time_t inRecyTime; //入到资源回收站里去的时间

	//和心跳包有关
	time_t lastPingTime; //上次ping的时间【上次发送心跳包的事件】

	//和网络安全有关
	uint64_t FloodkickLastTime;	 // Flood攻击上次收到包的时间
	int FloodAttackCount;		 // Flood攻击在该时间内收到包的次数统计
	std::atomic<int> iSendCount; //发送队列中有的数据条目数，若client只发不收，则可能造成此数过大，依据此数做出踢出处理

	//Todo本身的链表写法，现在已经不用了
	//NgxConnectionInfo *next; //这是个指针，指向下一个本类型对象，用于把空闲的连接池对象串起来构成一个单向链表，方便取用
};

//消息头，引入的目的是当收到数据包时，额外记录一些内容以备将来使用
struct NgxExtraMsgHeaderInfo
{
	NgxConnectionInfo *pConn; //记录对应的链接，注意这是个指针
	uint64_t iCurrsequence;	  //收到数据包时记录对应连接的序号，将来能用于比较是否连接已经作废用
};

//------------------------------------
// socket相关类
class CSocekt
{
public:
	CSocekt();						  
	virtual ~CSocekt();				   
	virtual bool Initialize(); //初始化函数[父进程中执行]
	virtual bool InitForSubProc(); //初始化函数[子进程中执行]
	virtual void ShutdownSubProc(); //关闭退出函数[子进程中执行]

	void printTDInfo(); //打印统计信息

public:
	virtual void ExecCmd(char *pMsgBuf);												//处理客户端请求，虚函数，因为将来可以考虑自己来写子类继承本类
	virtual void DoPingTimeOutChecking(NgxExtraMsgHeaderInfo *tmpmsg, time_t cur_time); //心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数只是把内存释放，子类应该重新事先该函数以实现具体的判断动作

public:
	// epoll功能初始化
	int NgxInitEpollAndConnPool(); 
	// epoll增加事件
	int NgxEpollProcessEvents(int timer); // epoll等待接收和处理事件
	// epoll操作事件
	int NgxEpollOperatorEvent(int fd, uint32_t eventtype, uint32_t flag, int bcaction, NgxConnectionInfo *pConn);
	
protected:
	//数据发送相关
	void _PushMsgToSendQueue(char *pSendBufPos);//把数据扔到待发送对列中
	//主动关闭一个连接时的要做些善后的处理函数
	void zdClosesocketProc(NgxConnectionInfo *p_Conn);

private:
	//监听端口相关
	bool _NgxOpenListeningSockets();	//监听必须的端口【支持多个端口】
	void _NgxCloselisteningSockets(); //关闭监听套接字
private:
	//accept连接以及可读可写事件通知。
	void _NgxAcceptConn(NgxConnectionInfo *oldc);//建立新连接
	void _NgxReadRequestHandler(NgxConnectionInfo *pConn);//设置数据来时的读处理函数
	void _NgxWriteRequestHandler(NgxConnectionInfo *pConn);//设置数据发送时的写处理函数
	
private:
	//接收解析包相关
	ssize_t _RecvFromClient(NgxConnectionInfo *pConn, char *buff, ssize_t buflen); //接收从客户端来的数据专用函数
	void _NgxRecvPkgHeadFinished(NgxConnectionInfo *pConn, bool &isflood);
	//包头收完整后的处理，我们称为包处理阶段1：
	void _NgxRecvWholePkgFinished(NgxConnectionInfo *pConn, bool &isflood);

	//对conn的接收包数据相关成员例如:状态和接收位置都复原
	void _ResetRecvPkgStatus(NgxConnectionInfo* pConn);
	
private:
	//发送数据相关
	ssize_t _SendToClient(NgxConnectionInfo *c, char *buff, ssize_t size); //将数据发送到客户端
	void _ClearMsgSendQueue(); //处理发送消息队列
private:
	//连接池/连接相关
	void _InitConnPool();//初始化连接池
	void _ClearConnPool();//回收连接池
	NgxConnectionInfo *_NgxGetFreeConn(int isock);//从连接池中获取一个空闲连接
	void _NgxFreeConnToFreeList(NgxConnectionInfo *pConn); //归还参数pConn所代表的连接到到连接池中
	void _inRecycleConnQueue(NgxConnectionInfo *pConn);	//将要回收的连接放到一个队列中来
	void _NgxFreeConnAndCloseConnFd(NgxConnectionInfo *pConn);	  //通用连接关闭函数，资源用这个函数释放
	
private:
	void ReadConf();					//专门用于读各种配置项
	bool _SetNonBlocking(int sockfd);	//设置非阻塞套接字

	//和网络安全有关
	bool _TestFlood(NgxConnectionInfo *pConn); //测试是否flood攻击成立，成立则返回true，否则返回false

private:
	//Todo:该函数暂时没人调用!!!
	//获取对端信息相关
	size_t _NgxSockNToP(struct sockaddr *sa, int port, u_char *text, size_t len); //根据参数1给定的信息，获取地址端口字符串，返回这个字符串的长度
	
private:
	//和时间相关的函数
	void _AddToTimerQueue(NgxConnectionInfo *pConn);			  //设置踢出时钟(向map表中增加内容)
	time_t _GetEarliestTime();								  //从multimap中取得最早的时间返回去
	NgxExtraMsgHeaderInfo *_RemoveFirstTimer();				  //从m_timeQueuemap移除最早的时间，并把最早这个时间所在的项的值所对应的指针 返回，调用者负责互斥，所以本函数不用互斥，
	NgxExtraMsgHeaderInfo *_GetOverTimeTimerr(time_t cur_time); //根据给的当前时间，从m_timeQueuemap找到比这个时间更老（更早）的节点【1个】返回去，这些节点都是时间超过了，要处理的节点
	void _DeleteConnFromTimerQueue(NgxConnectionInfo *pConn);	  //把指定用户tcp连接从timer表中抠出去
	void _ClearAllFromTimerQueue();							  //清理时间队列中所有内容
private:
	//线程相关函数
	static void *ServerSendQueueThread(void *threadData);		  //专门用来发送数据的线程
	static void *ServerRecyConnectionThread(void *threadData);	  //专门用来回收连接的线程
	static void *ServerTimerQueueMonitorThread(void *threadData); //时间队列监视线程，处理到期不发心跳包的用户踢出的线程

protected:
	//一些和网络通讯有关的成员变量
	size_t m_pkgHeaderSize; // sizeof(NgxPkgHeaderInfo);
	size_t m_pkgMsgHeaderSize; // sizeof(NgxExtraMsgHeaderInfo);

	//时间相关
	int m_ifTimeOutKick;//当时间到达Sock_MaxWaitTime指定的时间时,直接把客户端踢出去,只有当Sock_WaitTimeEnable = 1时，本项才有用
	int m_iWaitTime;//多少秒检测一次是否 心跳超时，只有当Sock_WaitTimeEnable = 1时，本项才有用

private:

	struct ThreadItemInSocket
	{
		pthread_t m_threadHandle; //线程句柄
		CSocekt *m_ptrSocket;   //记录线程池的指针
		bool m_bRunning;	   //标记是否正式启动起来，启动起来后，才允许调用StopAll()来释放

		ThreadItemInSocket(CSocekt *pthis) : m_ptrSocket(pthis), m_bRunning(false) {}
		~ThreadItemInSocket() {}
	};

	int m_workerProcessorConnMaxCnt; // epoll连接的最大项数
	int m_listenPortsCnt;	  //所监听的端口数量
	int m_epollHandle;		  // epoll_create返回的句柄

	//和连接池有关的
	std::list<NgxConnectionInfo *> m_allConnList;//连接列表【连接池】
	std::list<NgxConnectionInfo *> m_freeConnList; //空闲连接列表【这里边装的全是空闲的连接】
	std::atomic<int> m_totalConnCnt;//连接池总连接数
	std::atomic<int> m_freeConnCnt;//连接池空闲连接数
	pthread_mutex_t m_connMutex;//连接相关互斥量，互斥m_freeconnectionList，m_connectionList
	pthread_mutex_t m_recycleConnQueueMutex;//连接回收队列相关的互斥量
	std::list<NgxConnectionInfo *> m_recycleConnList; //将要释放的连接放这里
	std::atomic<int> m_recycleConnListSize;//待释放连接队列大小
	int m_recycleConnAfterTime;//等待这么些秒后才回收连接

	// NgxConnectionInfo* m_pfree_connections; //空闲连接链表头，连接池中总是有某些连接被占用，为了快速在池中找到一个空闲的连接，我把空闲的连接专门用该成员记录;

	std::vector<NgxListeningInfo *> m_listenSocketVec; //监听套接字队列
	struct epoll_event m_eventsArr[NGX_MAX_EVENTS];//用于放置epoll_wait()中返回的所发生的就绪事件数组

	//消息队列
	std::list<char *> m_sendMsgQueue;	   //发送数据消息队列
	std::atomic<int> m_sendMsgSize; //发消息队列大小
	//多线程相关
	std::vector<ThreadItemInSocket *> m_threadItemVec; //线程 容器，容器里就是各个线程了
	pthread_mutex_t m_sendMsgQueueMutex;  //发消息队列互斥量
	sem_t m_semEventSendQueue; //处理发消息线程相关的信号量

	//时间相关
	int m_bKickConnWhenTimeOut;//是否开启踢人时钟，1：开启   0：不开启
	pthread_mutex_t m_timeQueueMutex;	//和时间队列有关的互斥量
	std::multimap<time_t, NgxExtraMsgHeaderInfo *> m_timerQueueMap; //时间队列
	size_t m_timerQueueMapSize;//时间队列的尺寸
	time_t m_timerQueueFrontVal;//当前计时队列头部时间值

	//在线用户相关
	std::atomic<int> m_onlineUserCount; //当前在线用户数统计
	//网络安全相关
	int m_floodAkEnable;			  // Flood攻击检测是否开启,1：开启   0：不开启
	unsigned int m_floodTimeInterval; //表示每次收到数据包的时间间隔是100(毫秒)
	int m_floodAtkMaxCnt;			  //累积多少次踢出此人

	//统计用途
	time_t m_lastPrintTime;		//上次打印统计信息的时间(10秒钟打印一次)
	int m_iDiscardSendPkgCount; //丢弃的发送数据包数量
};

#endif
