
//和网络 中 时间 有关的函数放这里

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

//设置踢出时钟(向multimap表中增加内容)，用户三次握手成功连入，然后我们开启了踢人开关【Sock_WaitTimeEnable = 1】，那么本函数被调用；
void CSocekt::_AddToTimerQueue(NgxConnectionInfo* pConn)
{
    CMemory *p_memory = CMemory::GetInstance();

    time_t futtime = time(NULL);
    futtime += m_iWaitTime;  //20秒之后的时间

    CLock lock(&m_timeQueueMutex); //互斥，因为要操作m_timeQueuemap了
    NgxExtraMsgHeaderInfo* tmpMsgHeader = (NgxExtraMsgHeaderInfo*)p_memory->AllocMemory(m_pkgMsgHeaderSize,false);
    tmpMsgHeader->pConn = pConn;
    tmpMsgHeader->iCurrsequence = pConn->iCurrsequence;
    m_timerQueueMap.insert(std::make_pair(futtime,tmpMsgHeader)); //按键 自动排序 小->大
    m_timerQueueMapSize++;  //计时队列尺寸+1
    m_timerQueueFrontVal = _GetEarliestTime(); //计时队列头部时间值保存到m_timer_value_里
    return;    
}

//从multimap中取得最早的时间返回去，调用者负责互斥，所以本函数不用互斥，调用者确保m_timeQueuemap中一定不为空
time_t CSocekt::_GetEarliestTime()
{
    std::multimap<time_t, NgxExtraMsgHeaderInfo*>::iterator pos;	
	pos = m_timerQueueMap.begin();		
	return pos->first;	
}

//从m_timeQueuemap移除最早的时间，并把最早这个时间所在的项的值所对应的指针 返回，调用者负责互斥，所以本函数不用互斥，
NgxExtraMsgHeaderInfo* CSocekt::_RemoveFirstTimer()
{
	std::multimap<time_t, NgxExtraMsgHeaderInfo*>::iterator pos;	
	NgxExtraMsgHeaderInfo* p_tmp;
	if(m_timerQueueMapSize <= 0)
	{
		return NULL;
	}
	pos = m_timerQueueMap.begin(); //调用者负责互斥的，这里直接操作没问题的
	p_tmp = pos->second;
	m_timerQueueMap.erase(pos);
	--m_timerQueueMapSize;
	return p_tmp;
}

//根据给的当前时间，从m_timeQueuemap找到比这个时间更老（更早）的节点【1个】返回去，这些节点都是时间超过了，要处理的节点
//调用者负责互斥，所以本函数不用互斥
NgxExtraMsgHeaderInfo* CSocekt::_GetOverTimeTimerr(time_t cur_time)
{	
	CMemory *p_memory = CMemory::GetInstance();
	NgxExtraMsgHeaderInfo* ptmp;

	if (m_timerQueueMapSize == 0 || m_timerQueueMap.empty())
		return NULL; //队列为空

	time_t earliesttime = _GetEarliestTime(); //到multimap中去查询
	if (earliesttime <= cur_time)
	{
		//这回确实是有到时间的了【超时的节点】
		ptmp = _RemoveFirstTimer();    //把这个超时的节点从 m_timerQueueMap 删掉，并把这个节点的第二项返回来；

		if(/*m_ifkickTimeCount == 1 && */m_ifTimeOutKick != 1)  //能调用到本函数第一个条件肯定成立，所以第一个条件加不加无所谓，主要是第二个条件
		{
			//如果不是要求超时就提出，则才做这里的事：

			//因为下次超时的时间我们也依然要判断，所以还要把这个节点加回来        
			time_t newinqueutime = cur_time+(m_iWaitTime);
			NgxExtraMsgHeaderInfo* tmpMsgHeader = (NgxExtraMsgHeaderInfo*)p_memory->AllocMemory(sizeof(NgxExtraMsgHeaderInfo),false);
			tmpMsgHeader->pConn = ptmp->pConn;
			tmpMsgHeader->iCurrsequence = ptmp->iCurrsequence;			
			m_timerQueueMap.insert(std::make_pair(newinqueutime,tmpMsgHeader)); //自动排序 小->大			
			m_timerQueueMapSize++;       
		}

		if(m_timerQueueMapSize > 0) //这个判断条件必要，因为以后我们可能在这里扩充别的代码
		{
			m_timerQueueFrontVal = _GetEarliestTime(); //计时队列头部时间值保存到m_timer_value_里
		}
		return ptmp;
	}
	return NULL;
}

//把指定用户tcp连接从timer表中抠出去
void CSocekt::_DeleteFromTimerQueue(NgxConnectionInfo* pConn)
{
    std::multimap<time_t, NgxExtraMsgHeaderInfo*>::iterator pos,posend;
	CMemory *p_memory = CMemory::GetInstance();

    CLock lock(&m_timeQueueMutex);

    //因为实际情况可能比较复杂，将来可能还扩充代码等等，所以如下我们遍历整个队列找 一圈，而不是找到一次就拉倒，以免出现什么遗漏
lblMTQM:
	pos    = m_timerQueueMap.begin();
	posend = m_timerQueueMap.end();
	for(; pos != posend; ++pos)	
	{
		if(pos->second->pConn == pConn)
		{			
			p_memory->FreeMemory(pos->second);  //释放内存
			m_timerQueueMap.erase(pos);
			--m_timerQueueMapSize; //减去一个元素，必然要把尺寸减少1个;								
			goto lblMTQM;
		}		
	}
	if(m_timerQueueMapSize > 0)
	{
		m_timerQueueFrontVal = _GetEarliestTime();
	}
    return;    
}

//清理时间队列中所有内容
void CSocekt::_ClearAllFromTimerQueue()
{	
	std::multimap<time_t, NgxExtraMsgHeaderInfo*>::iterator pos,posend;

	CMemory *p_memory = CMemory::GetInstance();	
	pos    = m_timerQueueMap.begin();
	posend = m_timerQueueMap.end();    
	for(; pos != posend; ++pos)	
	{
		p_memory->FreeMemory(pos->second);		
		--m_timerQueueMapSize; 		
	}
	m_timerQueueMap.clear();
}

//时间队列监视和处理线程，处理到期不发心跳包的用户踢出的线程
void* CSocekt::ServerTimerQueueMonitorThread(void* threadData)
{
    ThreadItemInSocket *pThread = static_cast<ThreadItemInSocket*>(threadData);
    CSocekt *pSocketObj = pThread->m_ptrSocket;

    time_t absolute_time,cur_time;
    int err;

    while(g_processStopFlag == 0) //不退出
    {
        //这里没互斥判断，所以只是个初级判断，目的至少是队列为空时避免系统损耗		
		if(pSocketObj->m_timerQueueMapSize > 0)//队列不为空，有内容
        {
			//时间队列中最近发生事情的时间放到 absolute_time里；
            absolute_time = pSocketObj->m_timerQueueFrontVal; //这个可是省了个互斥，十分划算
            cur_time = time(NULL);
            if(absolute_time < cur_time)
            {
                //时间到了，可以处理了
                std::list<NgxExtraMsgHeaderInfo*> m_lsIdleList; //保存要处理的内容
                NgxExtraMsgHeaderInfo* result;

                err = pthread_mutex_lock(&pSocketObj->m_timeQueueMutex);  
                if(err != 0) ngx_log_stderr(err,"CSocekt::ServerTimerQueueMonitorThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);//有问题，要及时报告
                while ((result = pSocketObj->_GetOverTimeTimerr(cur_time)) != NULL) //一次性的把所有超时节点都拿过来
				{
					m_lsIdleList.push_back(result); 
				}//end while
                err = pthread_mutex_unlock(&pSocketObj->m_timeQueueMutex); 
                if(err != 0)  ngx_log_stderr(err,"CSocekt::ServerTimerQueueMonitorThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);//有问题，要及时报告                
                NgxExtraMsgHeaderInfo* tmpmsg;
                while(!m_lsIdleList.empty())
                {
                    tmpmsg = m_lsIdleList.front();
					m_lsIdleList.pop_front(); 
                    pSocketObj->DoPingTimeOutChecking(tmpmsg,cur_time); //这里需要检查心跳超时问题
                } //end while(!m_lsIdleList.empty())
            }
        } //end if(pSocketObj->m_timerQueueMapSize > 0)
        
        usleep(500 * 1000); //为简化问题，我们直接每次休息500毫秒
    } //end while

    return (void*)0;
}

//心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数只是把内存释放，子类应该重新事先该函数以实现具体的判断动作
void CSocekt::DoPingTimeOutChecking(NgxExtraMsgHeaderInfo* tmpmsg,time_t cur_time)
{
	CMemory *p_memory = CMemory::GetInstance();
	p_memory->FreeMemory(tmpmsg);    
}


