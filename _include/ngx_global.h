
#ifndef __NGX_GBLDEF_H__
#define __NGX_GBLDEF_H__



#include "ngx_c_slogic.h"
#include "ngx_c_threadpool.h"

//一些比较通用的定义放在这里，比如typedef定义
//一些全局变量的外部声明也放在这里

//类型定义----------------

//结构定义
struct ConfItem
{
	char ItemName[50];
	char ItemContent[500];
};

//和运行日志相关 
struct NgxLogInfo
{
	int    log_level;   //日志级别 或者日志类型，ngx_macro.h里分0-8共9个级别
	int    fd;          //日志文件描述符
};


//外部全局量声明
extern size_t        g_originArgvsMenSize;
extern size_t        g_envneedmem; 
extern int           g_originArgc; 
extern char          **g_ppOriginArgv;
extern char          *g_pNewEnvMem; 
extern int           g_isUsedDaemonMode;
extern CLogicSocket  g_socket;  
extern CThreadPool   g_threadpool;

extern pid_t         g_curPid;
extern pid_t         g_parentPid;
extern NgxLogInfo     g_ngxLogInfo;
extern int           g_curProcessType;   
extern sig_atomic_t  g_isWorkerProcessStatusChange;   
extern int           g_processStopFlag;

#endif
