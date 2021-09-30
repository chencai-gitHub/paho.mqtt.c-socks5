#include "LogTest.h"
#include <windows.h>
#include <stdio.h>

// ȫ�ֱ���
UINT32  g_iLogLevel = 0;    // ��־�ȼ�
UINT8   g_szLogFile[100] = { 0 };  // ��·������־�ļ���

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

	// ������־�ȼ�
	//if (iLogLevel > g_iLogLevel)
	//{
	//	return;
	//}

	//fp = fopen(g_szLogFile, "at+");      // ���ļ�, ÿ��д���ʱ���ں���׷��
	fp = fopen("C:\\pahoMQTT_log.txt", "at+");      // ���ļ�, ÿ��д���ʱ���ں���׷��
	if (fp == NULL)
	{
		return;
	}

	// д����־ʱ��
	GetTime(szTimeStr);
	fputs(szTimeStr, fp);

	// д����־����
	// ��ԭ�����������־�ȼ���ʶ
	//_snprintf(szLogContent, sizeof(szLogContent) - 1, "[WriteLog.c][%s]%s\n", LogLevel(iLogLevel), pszContent);

	_snprintf(szLogContent, sizeof(szLogContent) - 1, "[WriteLog.c][%s]%s\n", "_put_log_ : ", pszContent);
	fputs(szLogContent, fp);

	fflush(fp);     // ˢ���ļ�
	fclose(fp);     // �ر��ļ�
	fp = NULL;      // ���ļ�ָ����Ϊ��

	return;
}

