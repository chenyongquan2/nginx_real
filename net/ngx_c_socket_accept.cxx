
//和网络 中 接受连接【accept】 有关的函数放这里

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"

//建立新连接专用函数，当新连接进入时，本函数会被NgxWorkerProcessDealEventAndTimer()->NgxEpollProcessEvents()所调用
void CSocekt::_NgxAcceptConn(NgxConnectionInfo* pListenConnInfo)
{
    //ngx_log_stderr(0,"这是几个\n"); 这里会惊群，也就是说，epoll技术本身有惊群的问题
    //因为listen套接字上用的不是ET【边缘触发】，而是LT【水平触发】，意味着客户端连入如果我要不处理，这个函数会被多次调用，所以，我这里这里可以不必多次accept()，可以只执行一次accept()
    //这也可以避免本函数被卡太久，注意，本函数应该尽快返回，以免阻塞程序运行；
    
    static int use_accept4 = 1;   //我们先认为能够使用accept4()函数
    int newConnSocketFd = 0;
    struct sockaddr mysockaddr;        //远端服务器的socket地址
    socklen_t socklen = sizeof(mysockaddr);
    do 
    {     
        if(use_accept4)
        {
            //以为listen套接字是非阻塞的，所以即便已完成连接队列为空，accept4()也不会卡在这里；
            newConnSocketFd = accept4(pListenConnInfo->fd, &mysockaddr, &socklen, SOCK_NONBLOCK); //从内核获取一个用户端连接，最后一个参数SOCK_NONBLOCK表示返回一个非阻塞的socket，节省一次ioctl【设置为非阻塞】调用
        }
        else
        {
            //以为listen套接字是非阻塞的，所以即便已完成连接队列为空，accept()也不会卡在这里；
            newConnSocketFd = accept(pListenConnInfo->fd, &mysockaddr, &socklen);
        }

        //惊群，有时候不一定完全惊动所有4个worker进程，可能只惊动其中2个等等，其中一个成功其余的accept4()都会返回-1；错误 (11: Resource temporarily unavailable【资源暂时不可用】) 
        //所以参考资料：https://blog.csdn.net/russell_tao/article/details/7204260
        //其实，在linux2.6内核上，accept系统调用已经不存在惊群了（至少我在2.6.18内核版本上已经不存在）。大家可以写个简单的程序试下，在父进程中bind,listen，然后fork出子进程，
               //所有的子进程都accept这个监听句柄。这样，当新连接过来时，大家会发现，仅有一个子进程返回新建的连接，其他子进程继续休眠在accept调用上，没有被唤醒。
        //ngx_log_stderr(0,"测试惊群问题，看惊动几个worker进程%d\n",s); 【我的结论是：accept4可以认为基本解决惊群问题，但似乎并没有完全解决，有时候还会惊动其他的worker进程】

        /*
        if(newConnSocketFd == -1)
        {
            ngx_log_stderr(0,"惊群测试:_NgxAcceptConn()中accept失败,进程id=%d",g_curPid); 
        }
        else
        {
            ngx_log_stderr(0,"惊群测试:_NgxAcceptConn()中accept成功,进程id=%d",g_curPid); 
        } */       

        if(newConnSocketFd == -1)//accept失败了
        {
            int err = errno;
            //对accept、send和recv而言，事件未发生时errno通常被设置成EAGAIN（意为“再来一次”）或者EWOULDBLOCK（意为“期待阻塞”）
            if(err == EAGAIN) //accept()没准备好，这个EAGAIN错误EWOULDBLOCK是一样的
            {
                //除非你用一个循环不断的accept()取走所有的连接，不然一般不会有这个错误【我们这里只取一个连接，也就是accept()一次】
                return;
            } 
            int logLevel = NGX_LOG_ALERT;
            if (err == ECONNABORTED)  //ECONNRESET错误则发生在对方意外关闭套接字后【您的主机中的软件放弃了一个已建立的连接--由于超时或者其它失败而中止接连(用户插拔网线就可能有这个错误出现)】
            {
                //该错误被描述为“software caused connection abort”，即“软件引起的连接中止”。原因在于当服务和客户进程在完成用于 TCP 连接的“三次握手”后，
                    //客户 TCP 却发送了一个 RST （复位）分节，在服务进程看来，就在该连接已由 TCP 排队，等着服务进程调用 accept 的时候 RST 却到达了。
                    //POSIX 规定此时的 errno 值必须 ECONNABORTED。源自 Berkeley 的实现完全在内核中处理中止的连接，服务进程将永远不知道该中止的发生。
                        //服务器进程一般可以忽略该错误，直接再次调用accept。
                logLevel = NGX_LOG_ERR;
            } 
            else if (err == EMFILE || err == ENFILE) //EMFILE:进程的fd已用尽【已达到系统所允许单一进程所能打开的文件/套接字总数】。可参考：https://blog.csdn.net/sdn_prc/article/details/28661661   以及 https://bbs.csdn.net/topics/390592927
                                                        //ulimit -n ,看看文件描述符限制,如果是1024的话，需要改大;  打开的文件句柄数过多 ,把系统的fd软限制和硬限制都抬高.
                                                    //ENFILE这个errno的存在，表明一定存在system-wide的resource limits，而不仅仅有process-specific的resource limits。按照常识，process-specific的resource limits，一定受限于system-wide的resource limits。
            {
                logLevel = NGX_LOG_CRIT;
            }
            //ngx_log_error_core(logLevel,errno,"CSocekt::_NgxAcceptConn()中accept4()失败!");

            if(use_accept4 && err == ENOSYS) //accept4()函数没实现，坑爹？
            {
                use_accept4 = 0;  //标记不使用accept4()函数，改用accept()函数
                continue;         //回去重新用accept()函数搞
            }

            if (err == ECONNABORTED)  //对方关闭套接字
            {
                //这个错误因为可以忽略，所以不用干啥
            }
            else if (err == EMFILE || err == ENFILE) 
            {
                //do nothing，这个官方做法是先把读事件从listen socket上移除，然后再弄个定时器，定时器到了则继续执行该函数，但是定时器到了有个标记，会把读事件增加到listen socket上去；
                //我这里目前先不处理吧【因为上边已经写这个日志了】；
            }            
            return;
        } 

        //走到这里的，表示accept4()/accept()成功了       

        //检查是否能分配一个新连接???
        {
            //判断当前活跃/在线连接数是否过多 
            if(m_onlineUserCount >= m_workerProcessorConnMaxCnt)
            {
                //ngx_log_stderr(0,"超出系统允许的最大连入用户数(最大允许连入数%d)，关闭连入请求(%d)。",m_workerProcessorConnMaxCnt,s);  
                close(newConnSocketFd);
                return ;
            }
            //判断是否存在大量恶意连接(发了1条数据就断，不断连接)
            //会导致频繁调用ngx_get_connection()使用我们短时间内产生大量连接，危及本服务器安全
            if(m_allConnList.size() > (m_workerProcessorConnMaxCnt * 5))
            {
                //比如你允许同时最大2048个连接，但连接池却有了 2048*5这么大的容量，这肯定是表示短时间内 产生大量连接/断开，因为我们的延迟回收机制，这里连接还在垃圾池里没有被回收
                if(m_freeConnList.size() < m_workerProcessorConnMaxCnt)
                {
                    //整个连接池这么大了，而空闲连接却这么少了，所以我认为是 短时间内 产生大量连接，发一个包后就断开，我们不可能让这种情况持续发生，所以必须断开新入用户的连接
                    //一直等到m_freeconnectionList变得足够大【连接池中连接被回收的足够多】,才允许新连接接上。
                    close(newConnSocketFd);
                    return ;   
                }
            }
        }
        
        //通过检查 可以分配一个新连接。
        //ngx_log_stderr(errno,"accept4成功s=%d",newConnSocketFd);
        NgxConnectionInfo* pNewConn = _NgxGetFreeConn(newConnSocketFd); //这是针对新连入用户的连接，和监听套接字 所对应的连接是两个不同的东西，不要搞混
        if(pNewConn == NULL)
        {
            //连接池中连接不够用，那么就得把这个socekt直接关闭并返回了，因为在ngx_get_connection()中已经写日志了，所以这里不需要写日志了
            if(close(newConnSocketFd) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocekt::_NgxAcceptConn()中close(%d)失败!",newConnSocketFd);                
            }
            return;
        }

        //成功的拿到了连接池中的一个连接
        memcpy(&pNewConn->s_sockaddr,&mysockaddr,socklen);  //拷贝客户端地址到连接对象【要转成字符串ip地址参考函数ngx_sock_ntop()】
        /*
        //{
        //    //测试将收到的地址弄成字符串，格式形如"192.168.1.126:40904"或者"192.168.1.126"
        //    u_char ipaddr[100]; memset(ipaddr,0,sizeof(ipaddr));
        //    _NgxSockNToP(&pNewConn->s_sockaddr,1,ipaddr,sizeof(ipaddr)-10); //宽度给小点
        //    ngx_log_stderr(0,"ip信息为%s\n",ipaddr);
        //}*/

        if(!use_accept4)
        {
            //如果不是用accept4()取得的socket，那么就要设置为非阻塞【因为用accept4()的已经被accept4()设置为非阻塞了】
            if(_SetNonBlocking(newConnSocketFd) == false)
            {
                 //关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟
                _NgxFreeConnAndCloseConnFd(pNewConn);
                return; //直接返回
            }
        }

        pNewConn->listening = pListenConnInfo->listening;//连接对象 和监听对象关联，方便通过连接对象找监听对象【关联到监听端口】
        //pNewConn->w_ready = 1;//标记可以写，新连接写事件肯定是ready的；【从连接池拿出一个连接时这个连接的所有成员都是0】            
        
        pNewConn->rhandler = &CSocekt::_NgxReadRequestHandler;  //设置数据来时的读处理函数
        pNewConn->whandler = &CSocekt::_NgxWriteRequestHandler; //设置数据发送时的写处理函数

        //把三次握手后建立的连接socket给添加到epoll里面。 
        if(NgxEpollOperatorEvent(
            newConnSocketFd,     //socekt句柄
            EPOLL_CTL_ADD,      //事件类型，这里是增加
            EPOLLIN|EPOLLRDHUP, //标志，这里代表要增加的标志,EPOLLIN：可读，EPOLLRDHUP：TCP连接的远端关闭或者半关闭 ，如果边缘触发模式可以增加 EPOLLET
            0,                  //对于事件类型为增加的，不需要这个参数
            pNewConn             //连接池中的连接
        ) == -1)         
        {
            //增加事件失败，失败日志在ngx_epoll_add_event中写过了，因此这里不多写啥；
            _NgxFreeConnAndCloseConnFd(pNewConn);//关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟；
            return; //直接返回
        }
        /*
        else
        {
            //打印下发送缓冲区大小
            int           n;
            socklen_t     len;
            len = sizeof(int);
            getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
            ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040

            n = 0;
            getsockopt(s,SOL_SOCKET,SO_RCVBUF, &n, &len);
            ngx_log_stderr(0,"接收缓冲区的大小为%d!",n); //374400

            int sendbuf = 2048;
            if (setsockopt(s, SOL_SOCKET, SO_SNDBUF,(const void *) &sendbuf,n) == 0)
            {
                ngx_log_stderr(0,"发送缓冲区大小成功设置为%d!",sendbuf); 
            }

             getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
            ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040
        }
        */

        //将新建立的链接给加入到定时检测队列里面。
        if(m_bKickConnWhenTimeOut == 1)
        {
            _AddToTimerQueue(pNewConn);
        }
        ++m_onlineUserCount;  //连入用户数量+1        
        break;  //一般就是循环一次就跳出去
    } while (1);   

    return;
}

