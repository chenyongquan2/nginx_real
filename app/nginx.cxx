
#include "ngx_stdfx.h"

#include "ngx_macro.h"         //各种宏定义
#include "ngx_func.h"          //各种函数声明
#include "ngx_c_conf.h"        //和配置文件处理相关的类,名字带c_表示和类有关
#include "ngx_c_socket.h"      //和socket通讯相关
#include "ngx_c_memory.h"      //和内存分配释放等相关
#include "ngx_c_threadpool.h"  //和多线程有关
#include "ngx_c_crc32.h"       //和crc32校验算法有关 
#include "ngx_c_slogic.h"      //和socket通讯相关

//和设置标题有关的全局量
size_t  g_originArgvsMenSize=0;        //保存下这些argv参数所需要的内存大小
size_t  g_envneedmem=0;         //环境变量所占内存大小
int     g_originArgc;              //参数个数 
char    **g_ppOriginArgv;            //原始命令行参数数组,在main中会被赋值
char    *g_pNewEnvMem=NULL;        //指向自己分配的env环境变量的内存，在ngx_init_setproctitle()函数中会被分配内存
int     g_isUsedDaemonMode=0;         //守护进程标记，标记是否启用了守护进程模式，0：未启用，1：启用了

//socket/线程池相关
//CSocekt      g_socket;          //socket全局对象
CLogicSocket   g_socket;        //socket全局对象  
CThreadPool    g_threadpool;    //线程池全局对象

//和进程本身有关的全局量
pid_t g_curPid;//当前进程的pid
pid_t g_parentPid;//父进程的pid
int g_curProcessType;//进程类型，比如master,worker进程等
int g_processStopFlag;//标志程序退出,0不退出1，退出

sig_atomic_t  g_isWorkerProcessStatusChange;         //标记子进程状态变化[一般是子进程发来SIGCHLD信号表示退出],sig_atomic_t:系统定义的类型：访问或改变这些变量需要在计算机的一条指令内完成
                                   //一般等价于int【通常情况下，int类型的变量通常是原子访问的，也可以认为 sig_atomic_t就是int类型的数据】  

NgxLogInfo g_ngxLogInfo;//全局的日志变量。

struct MainLoopGarbageGuard
{
    MainLoopGarbageGuard(){};
    ~MainLoopGarbageGuard()
    {
        if(g_pNewEnvMem)
        {
            delete []g_pNewEnvMem;
            g_pNewEnvMem = nullptr;
        }

        //关闭日志文件
        if(g_ngxLogInfo.fd != STDERR_FILENO && g_ngxLogInfo.fd != -1)  
        {        
            close(g_ngxLogInfo.fd);
            g_ngxLogInfo.fd = -1; //标记下，防止被再次close吧        
        }
    }
};                               

void initGlobalMem(int argc, char *const *argv)
{
    g_processStopFlag = 0;//标记程序是否退出，0不退出          

    //(1)无伤大雅也不需要释放的放最上边    
    g_curPid    = getpid();      //取得进程pid
    g_parentPid = getppid();     //取得父进程的id 

    //统计argv所占的内存
    g_originArgvsMenSize = 0;
    for(int i = 0; i < argc; i++)  //argv =  ./nginx -a -b -c asdfas
        g_originArgvsMenSize += strlen(argv[i]) + 1; //+1是给\0留空间。

    //统计环境变量所占的内存。注意判断方法是environ[i]是否为空作为环境变量结束标记
    for(int i = 0; environ[i]; i++) 
        g_envneedmem += strlen(environ[i]) + 1; //+1是因为末尾有\0,是占实际内存位置的，要算进来


    g_originArgc = argc;           //保存参数个数
    g_ppOriginArgv = (char **) argv; //保存参数指针

    //全局量有必要初始化的
    g_ngxLogInfo.fd = -1;                  //-1：表示日志文件尚未打开；因为后边ngx_log_stderr要用所以这里先给-1
    g_curProcessType = NGX_PROCESS_MASTER; //先标记本进程是master进程
    g_isWorkerProcessStatusChange = 0;                     //标记子进程没有发生变化
}

int main(int argc, char *const *argv)
{     
    //printf("%u,%u,%u",EPOLLERR ,EPOLLHUP,EPOLLRDHUP);  
    MainLoopGarbageGuard guard;//RAII释放main的资源
    
    //(0)先初始化的变量
    initGlobalMem(argc, argv);

    int exitcode = 0;
   
    //(2)初始化失败，就要直接退出的
    //配置文件必须最先要，后边初始化啥的都用，所以先把配置读出来，供后续使用 
    CConfig *p_config = CConfig::GetInstance(); //单例类
    if(p_config->Load("nginx.conf") == false) //把配置文件内容载入到内存            
    {   
        ngx_log_init();    //初始化日志
        ngx_log_stderr(0,"配置文件[%s]载入失败，退出!","nginx.conf");
        //exit(1);终止进程，在main中出现和return效果一样 ,exit(0)表示程序正常, exit(1)/exit(-1)表示程序异常退出，exit(2)表示表示系统找不到指定的文件
        exitcode = 2; //标记找不到文件
        goto lblexit;
    }
    
    CMemory::GetInstance();	//(2.1)内存单例类可以在这里初始化，返回值不用保存
    CCRC32::GetInstance();//(2.2)crc32校验算法单例类可以在这里初始化，返回值不用保存
        
    //(3)一些必须事先准备好的资源，先初始化
    ngx_log_init();//日志初始化(创建/打开日志文件)，这个需要配置项，所以必须放配置文件载入的后边；     
        
    //(4)一些初始化函数       
    if(NgxRegisterSignalsHandle() != 0) //信号处理函数注册初始化
    {
        exitcode = 1;
        goto lblexit;
    }        
    if(g_socket.Initialize() == false)//创建listen socket(socket+bind+listen监听端口)
    {
        exitcode = 1;
        goto lblexit;
    }

    //(5)一些不好归类的其他类别的代码
    NgxMoveEnvironMemPos();//设置标题之前先对原本环境变量的内存进行搬家,方便后面进行修改进程的标题

    //(6)创建守护进程
    if(p_config->GetIntDefault("Daemon",0) == 1) //读配置文件，拿到配置文件中是否按守护进程方式启动的选项
    {
        int res = NgxForkAndCreateDaemon();
        if(res == -1) //fork()失败
        {
            exitcode = 1;
            goto lblexit;
        }
        if(res == 1)
        {
            //把最原始的父进程给退出，因为已经让了子进程(守护进程)成为了进程组组长了
            exitcode = 0;//而我现在这个情况属于正常fork()守护进程后的正常退出，不应该跑到lblexit()去执行，因为那里有一条打印语句标记整个进程的退出，这里不该限制该条打印语句；
            return exitcode; 
        }
        //走到这里，成功创建了守护进程并且这里已经是fork()出来的进程，现在这个进程做了master进程
        g_isUsedDaemonMode = 1;//标记是否启用了守护进程模式
    }

    //(7)开始正式的主工作流程   
    NgxMasterProcessCycleLoop(); //不管父master进程还是子worker进程，正常工作期间都在这个函数里循环；
        
    //--------------------------------------------------------------    
    //for(;;)    
    //{
    //    sleep(1); //休息1秒        
    //    printf("休息1秒\n");        
    //}
      
    //--------------------------------------
lblexit:
    //(5)该释放的资源要释放掉
    ngx_log_stderr(0,"程序退出，再见了!");
    //printf("程序退出，再见!\n");    
    return exitcode;
}
