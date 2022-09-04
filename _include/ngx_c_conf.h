
#ifndef __NGX_CONF_H__
#define __NGX_CONF_H__

#include "ngx_stdfx.h"
#include "ngx_global.h"  //一些全局/通用定义

class CConfig
{
private:
	CConfig();
public:
	~CConfig();
private:
	static CConfig *m_instance;

public:	
	//单例类
	static CConfig* GetInstance() 
	{	
		if(m_instance == NULL)
		{
			//锁
			if(m_instance == NULL)
			{					
				m_instance = new CConfig();
				static ConfigMemGuard c; 
			}
			//放锁		
		}
		return m_instance;
	}	
	class ConfigMemGuard  //类中套类，用于释放对象
	{
	public:				
		~ConfigMemGuard()
		{
			if (CConfig::m_instance)
			{						
				delete CConfig::m_instance;				
				CConfig::m_instance = NULL;				
			}
		}
	};
//---------------------------------------------------
public:
    bool Load(const char *pConfFileName); //装载配置文件
	const char *GetString(const char *pItemname);
	int  GetIntDefault(const char *pItemname, const int def);

public:
	std::vector<ConfItem*> m_configItemList; //存储配置信息的列表

};

#endif
