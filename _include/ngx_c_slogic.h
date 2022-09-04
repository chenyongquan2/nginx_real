
#ifndef __NGX_C_SLOGIC_H__
#define __NGX_C_SLOGIC_H__

#include "ngx_stdfx.h"
#include "ngx_c_socket.h"

//处理逻辑和通讯的子类
class CLogicSocket : public CSocekt   //继承自父类CScoekt
{
public:
	CLogicSocket();                                                         //构造函数
	virtual ~CLogicSocket();                                                //释放函数
	virtual bool Initialize();                                              //初始化函数

	//通用收发数据相关函数
	void SendNoBodyPkgToClient(NgxExtraMsgHeaderInfo* pMsgHeader, unsigned short iMsgCode);
	//心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数只是把内存释放，子类应该重新事先该函数以实现具体的判断动作
	virtual void DoPingTimeOutChecking(NgxExtraMsgHeaderInfo* tmpmsg, time_t cur_time);      

public:
	virtual void ExecCmd(char *pMsgBuf);
	//各种业务逻辑相关函数都在之类
	bool DoCmdRegister(NgxConnectionInfo* pConn, NgxExtraMsgHeaderInfo* pMsgHeader, char *pPkgBody, unsigned short iBodyLength);
	bool DoCmdLogin(NgxConnectionInfo* pConn, NgxExtraMsgHeaderInfo* pMsgHeader, char *pPkgBody, unsigned short iBodyLength);
	bool DoCmdPing(NgxConnectionInfo* pConn, NgxExtraMsgHeaderInfo* pMsgHeader, char *pPkgBody, unsigned short iBodyLength);
};

#endif
