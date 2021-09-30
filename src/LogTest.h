#ifndef LOG_TEST_H
#define LOG_TEST_H

typedef signed   int    INT32;
typedef unsigned int    UINT32;
typedef unsigned char   UINT8;

// 宏定义
#define LOG_FATAL       0     // 严重错误
#define LOG_ERROR       1     // 一般错误
#define LOG_WARN        2     // 警告
#define LOG_INFO        3     // 一般信息
#define LOG_TRACE       4     // 跟踪信息
#define LOG_DEBUG       5     // 调试信息
#define LOG_ALL         6     // 所有信息


// 函数声明
void   WriteLogFile(UINT32 iLogLevel, UINT8 *pszContent);
UINT8 *LogLevel(UINT32 iLogLevel);
void   GetTime(UINT8 *pszTimeStr);

#endif