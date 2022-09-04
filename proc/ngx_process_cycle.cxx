//和开启子进程相关

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

//函数声明
static void NgxCreateWorkerProcesses(int threadnums);
static int NgxCreateWorkerProcess(int threadnums,const char *pProcName);
static void NgxWorkerProcessCycleLoop(int inum,const char *pProcName);
static void NgxWorkerProcessInit(int inum);

static u_char master_process[] = "master process";

static void doSetMasterProcessTitle()
{
    //设置主进程标题
    size_t size = sizeof(master_process);  //注意我这里用的是sizeof，所以字符串末尾的\0是被计算进来了的
    size += g_originArgvsMenSize;          //argv参数长度加进来    
    if(size < 1000) //长度小于这个，我才设置标题
    {
        char title[1000] = {0};
        strcpy(title,(const char *)master_process); //"master process"
        strcat(title," ");  //跟一个空格分开一些，清晰    //"master process "
        for (int i = 0; i < g_originArgc; i++)         //"master process ./nginx"
            strcat(title,g_ppOriginArgv[i]);
        
        NgxSetProcessTitle(title); //设置标题
        ngx_log_error_core(NGX_LOG_NOTICE,0,"%s %P 【master进程】启动并开始运行......!",title,g_curPid); //设置标题时顺便记录下来进程名，进程id等信息到日志
    }    
}


//描述：创建worker子进程
void NgxMasterProcessCycleLoop()
{    
    //防止fork()期间收到信号导致混乱,所以在fork创建子进程过程中 尽量屏蔽掉信号，防止fork被信号中断。
    sigset_t set;        //信号集
    sigemptyset(&set);   //清空信号集
    //下列这些信号在执行本函数期间不希望收到【考虑到官方nginx中有这些信号，老师就都搬过来了】（保护不希望由信号中断的代码临界区）
    //建议fork()子进程时学习这种写法，防止信号的干扰；
    sigaddset(&set, SIGCHLD);     //子进程状态改变
    sigaddset(&set, SIGALRM);     //定时器超时
    sigaddset(&set, SIGIO);       //异步I/O
    sigaddset(&set, SIGINT);      //终端中断符
    sigaddset(&set, SIGHUP);      //连接断开
    sigaddset(&set, SIGUSR1);     //用户定义信号
    sigaddset(&set, SIGUSR2);     //用户定义信号
    sigaddset(&set, SIGWINCH);    //终端窗口大小改变
    sigaddset(&set, SIGTERM);     //终止
    sigaddset(&set, SIGQUIT);     //终端退出符
    //可以根据开发的实际需要往其中添加其他要屏蔽的信号
    
    //设置，此时无法接受的信号；阻塞期间，你发过来的上述信号，多个会被合并为一个，暂存着，等你放开信号屏蔽后才能收到这些信号。。。
    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) //第一个参数用了SIG_BLOCK表明设置 进程 新的信号屏蔽字 为 “当前信号屏蔽字 和 第二个参数指向的信号集的并集
    {        
        ngx_log_error_core(NGX_LOG_ALERT,errno,"NgxMasterProcessCycleLoop()中sigprocmask()失败!");
    }

    doSetMasterProcessTitle();
        
    //从配置文件中读取要创建的worker进程数量
    CConfig *p_config = CConfig::GetInstance();
    int workprocess = p_config->GetIntDefault("WorkerProcesses",1); 
    NgxCreateWorkerProcesses(workprocess);  //这里要创建worker子进程

    //创建子进程后，父进程的执行流程会返回到这里，子进程不会走进来    
    sigemptyset(&set); //信号屏蔽字为空，表示不屏蔽任何信号
    
    while(1) 
    {
        //usleep(100000);
        //ngx_log_error_core(0,0,"haha--这是父进程，pid为%P",g_curPid);

        // sigsuspend(const sigset_t *mask))用于在接收到某个信号之前, 临时用mask替换进程的信号掩码, 并暂停进程执行，直到收到信号为止。
        // sigsuspend 返回后将恢复调用之前的信号掩码。信号处理函数完成后，进程将继续执行。该系统调用始终返回-1，并将errno设置为EINTR。

        //sigsuspend是一个原子操作，包含4个步骤：
        //a)根据给定的参数设置新的mask 并 阻塞当前进程【因为是个空集，所以不阻塞任何信号】
        //b)此时，一旦收到信号，便恢复原先的信号屏蔽【我们原来调用sigprocmask()的mask在上边设置的，阻塞了多达10个信号，从而保证我下边的执行流程不会再次被其他信号截断】
        //c)调用该信号对应的信号处理函数
        //d)信号处理函数返回后，sigsuspend返回，使程序流程继续往下走
        //printf("for进来了！\n"); //发现，如果print不加\n，无法及时显示到屏幕上，是行缓存问题，以往没注意；可参考https://blog.csdn.net/qq_26093511/article/details/53255970

        sigsuspend(&set); //阻塞在这里，等待一个信号，此时进程是挂起的，不占用cpu时间，只有收到信号才会被唤醒（返回）；
                         //此时master进程完全靠信号驱动干活    

        //printf("执行到sigsuspend()下边来了\n");
        
        //printf("master进程休息1秒\n");      
        //ngx_log_stderr(0,"haha--这是父进程，pid为%P",g_curPid); 
        sleep(1); //休息1秒        
    }
    return;
}

//根据给定的参数创建指定数量的子进程
static void NgxCreateWorkerProcesses(int threadnums)
{
    for (int i = 0; i < threadnums; i++) //master进程在走这个循环，来创建若干个子进程
    {
        NgxCreateWorkerProcess(i,"worker process");
    } 
}

//产生一个子进程
//pprocname：子进程名字"worker process"
static int NgxCreateWorkerProcess(int inum, const char *pProcName)
{
    pid_t pid = fork();
    switch (pid)  //pid判断父子进程，分支处理
    {  
    case -1: //产生子进程失败
        ngx_log_error_core(NGX_LOG_ALERT,errno,"NgxCreateWorkerProcess()fork()产生子进程num=%d,procname=\"%s\"失败!",inum,pProcName);
        return -1;

    case 0:  //子进程分支
        g_parentPid = g_curPid;//因为是子进程了，所有原来的pid变成了父pid
        g_curPid = getpid();//重新获取pid,即本子进程的pid
        NgxWorkerProcessCycleLoop(inum,pProcName);    //我希望所有worker子进程，在这个函数里不断循环着不出来，也就是说，子进程流程不往下边走;
        break;

    default://父进程      
        break;
    }

    //父进程分支在这里返回,子进程流程不往下边走.
}

//worker子进程的功能函数，每个woker子进程，就在这里死循环着了【处理网络事件和定时器事件以对外提供web服务】
static void NgxWorkerProcessCycleLoop(int inum,const char *pProcName) 
{
    //设置进程的类型，是worker进程
    g_curProcessType = NGX_PROCESS_WORKER;

    //Todo:下面的为重要函数:1清空信号集 2创建线程池 3初始化线程相关的互斥量条件变量等
    NgxWorkerProcessInit(inum);

    NgxSetProcessTitle(pProcName);//重新为子进程设置进程名
    ngx_log_error_core(NGX_LOG_NOTICE,0,"%s %P 【worker进程】启动并开始运行......!",pProcName,g_curPid); //设置标题时顺便记录下来进程名，进程id等信息到日志

    //测试代码，测试线程池的关闭
    //sleep(5); //休息5秒        
    //g_threadpool.StopAll(); //测试Create()后立即释放的效果

    //暂时先放个死循环，我们在这个循环里一直不出来
    //setvbuf(stdout,NULL,_IONBF,0); //这个函数. 直接将printf缓冲区禁止， printf就直接输出了。
    while(1)
    {

        //先sleep一下 以后扩充.......
        //printf("worker进程休息1秒");       
        //fflush(stdout); //刷新标准输出缓冲区，把输出缓冲区里的东西打印到标准输出设备上，则printf里的东西会立即输出；
        //sleep(1); //休息1秒       
        //usleep(100000);
        //ngx_log_error_core(0,0,"good--这是子进程，编号为%d,pid为%P！",inum,g_curPid);
        //printf("1212");
        //if(inum == 1)
        //{
            //ngx_log_stderr(0,"good--这是子进程，编号为%d,pid为%P",inum,g_curPid); 
            //printf("good--这是子进程，编号为%d,pid为%d\r\n",inum,g_curPid);
            //ngx_log_error_core(0,0,"good--这是子进程，编号为%d",inum,g_curPid);
            //printf("我的测试哈inum=%d",inum++);
            //fflush(stdout);
        //}
            
        //ngx_log_stderr(0,"good--这是子进程，编号为%d,pid为%P",inum,g_curPid); 
        //ngx_log_error_core(0,0,"good--这是子进程，编号为%d,pid为%P",inum,g_curPid);

        NgxWorkerProcessDealEventAndTimer(); //处理网络事件和定时器事件

        /*if(false) //优雅的退出
        {
            g_processStopFlag = 1;
            break;
        }*/

    } //end for(;;)

    g_threadpool.StopAll();      //考虑在这里停止线程池；
    g_socket.ShutdownSubProc(); //socket需要释放的东西考虑释放；
    return;
}

//描述：子进程创建时调用本函数进行一些初始化工作
static void NgxWorkerProcessInit(int inum)
{
    sigset_t set;//信号集
    sigemptyset(&set);//清空信号集
    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1)//原来是屏蔽那10个信号【防止fork()期间收到信号导致混乱】，现在不再屏蔽任何信号【接收任何信号】
        ngx_log_error_core(NGX_LOG_ALERT,errno,"NgxWorkerProcessInit()中sigprocmask()失败!");

    //线程池代码，率先创建，至少要比和socket相关的内容优先
    CConfig *p_config = CConfig::GetInstance();
    int threadNum = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount",5); //处理接收到的消息的线程池中线程数量
    if(g_threadpool.Create(threadNum) == false)  //创建线程池中线程
        exit(-2);//内存没释放，但是简单粗暴退出；
    
    sleep(1); //再休息1秒；

    //1初始化子进程需要具备的一些多线程能力相关的信息 2并且创建相关的线程(这些线程启动后会分别阻塞等待对应的信号/信号量/通知)
    if(g_socket.InitForSubProc() == false) 
        exit(-2);

    //1初始化epoll相关内容，2.初始化连接池 3同时往监听listen socket分配连接conn 并且添加到epoll
    //思考:由于listen socket是在主进程里面创建出来的，
    //所以主进程和四个进程都会有这个listen socket，
    //但是只有子进程将其添加进去了epoll，因此只有子进程才会收到这些监听socket的事件.
    g_socket.NgxInitEpollAndConnPool(); 
    
}
