//和开启子进程相关

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

//处理网络事件和定时器事件
void NgxWorkerProcessDealEventAndTimer()
{
    g_socket.NgxEpollProcessEvents(-1); //-1表示一直无限等待直到有事件来
    g_socket.printTDInfo(); //统计信息打印，考虑到测试的时候总会收到各种数据信息，所以上边的函数调用一般都不会卡住等待收数据
}

