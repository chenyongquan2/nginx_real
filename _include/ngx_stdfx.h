#ifndef __NGX_STDFX_H__
#define __NGX_STDFX_H__

//c语言头文件
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>    //va_start....
#include <time.h>      //localtime_r
#include <fcntl.h>     //open




//linux专属
#include <unistd.h>    //STDERR_FILENO等 env usleep
#include <errno.h>
#include <signal.h> 

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>  //epoll
#include <sys/time.h>  //gettimeofday
#include <sys/ioctl.h> //ioctl
#include <sys/stat.h>
#include <sys/wait.h>  //waitpid

//c++相关头文件
#include <iostream>
#include <vector>
#include <list>
#include <map>          //multimap

//线程相关头文件
#include <pthread.h> 
#include <semaphore.h>  //信号量 
#include <atomic>       //c++11里的原子操作

#endif //__NGX_STDFX_H__