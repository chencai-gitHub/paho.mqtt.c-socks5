#include "LogTest.h"
#include <windows.h>
#include <stdio.h>

// 全局变量
UINT32  g_iLogLevel = 0;    // 日志等级
UINT8   g_szLogFile[100] = { 0 };  // 带路径的日志文件名

void GetTime(UINT8 *pszTimeStr)
{
	SYSTEMTIME  tSysTime = { 0 };

	GetLocalTime(&tSysTime);
	sprintf(pszTimeStr, "[%04d.%02d.%02d %02d:%02d:%02d.%03d]",
		tSysTime.wYear, tSysTime.wMonth, tSysTime.wDay,
		tSysTime.wHour, tSysTime.wMinute, tSysTime.wSecond,
		tSysTime.wMilliseconds);

	return;
}

UINT8 *LogLevel(UINT32 iLogLevel)
{
	switch (iLogLevel)
	{
	case LOG_FATAL:
	{
		return "FATAL";
	}

	case LOG_ERROR:
	{
		return "ERROR";
	}

	case LOG_WARN:
	{
		return "WARN";
	}

	case LOG_INFO:
	{
		return "INFO";
	}

	case LOG_TRACE:
	{
		return "TRACE";
	}

	case LOG_DEBUG:
	{
		return "DEBUG";
	}

	case LOG_ALL:
	{
		return "ALL";
	}

	default:
	{
		return "OTHER";
	}
	}
}

void WriteLogFile(UINT32 iLogLevel, UINT8 *pszContent)
{
	FILE  *fp = NULL;
	UINT8  szLogContent[2048] = { 0 };
	UINT8  szTimeStr[128] = { 0 };

	if (pszContent == NULL)
	{
		return;
	}

	// 过滤日志等级
	//if (iLogLevel > g_iLogLevel)
	//{
	//	return;
	//}

	//fp = fopen(g_szLogFile, "at+");      // 打开文件, 每次写入的时候在后面追加
	fp = fopen("C:\\pahoMQTT_log.txt", "at+");      // 打开文件, 每次写入的时候在后面追加
	if (fp == NULL)
	{
		return;
	}

	// 写入日志时间
	GetTime(szTimeStr);
	fputs(szTimeStr, fp);

	// 写入日志内容
	// 在原内容中添加日志等级标识
	//_snprintf(szLogContent, sizeof(szLogContent) - 1, "[WriteLog.c][%s]%s\n", LogLevel(iLogLevel), pszContent);

	_snprintf(szLogContent, sizeof(szLogContent) - 1, "[WriteLog.c][%s]%s\n", "_put_log_ : ", pszContent);
	fputs(szLogContent, fp);

	fflush(fp);     // 刷新文件
	fclose(fp);     // 关闭文件
	fp = NULL;      // 将文件指针置为空

	return;
}

