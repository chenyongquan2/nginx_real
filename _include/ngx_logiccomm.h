
#ifndef __NGX_LOGICCOMM_H__
#define __NGX_LOGICCOMM_H__

//收发命令宏定义

#define _CMD_START	                    0  
#define _CMD_PING				   	    _CMD_START + 0   //ping命令【心跳包】
#define _CMD_REGISTER 		            _CMD_START + 5   //注册
#define _CMD_LOGIN 		                _CMD_START + 6   //登录



//结构定义------------------------------------
#pragma pack (1) //对齐方式,1字节对齐【结构之间成员不做任何字节对齐：紧密的排列在一起】

struct RegisterInfo
{
	int           iType;          //类型
	char          username[56];   //用户名 
	char          password[40];   //密码
};

struct LoginInfo
{
	char          username[56];   //用户名 
	char          password[40];   //密码
};


#pragma pack() //取消指定对齐，恢复缺省对齐

#endif
