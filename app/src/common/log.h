#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <string>

class Log
{
public:
	Log(const std::string &funcName)
	{
		printf("[APP] %s: ", funcName.c_str());
	}
	
	template <class T>
	Log &operator<<(const T &v)
	{
		return *this;
	}
	
	~Log()
	{
		printf("\n");
	}
};

#define DEBUGLOG Log(__FUNCTION__)

#endif
