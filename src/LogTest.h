#ifndef LOG_TEST_H
#define LOG_TEST_H

typedef signed   int    INT32;
typedef unsigned int    UINT32;
typedef unsigned char   UINT8;

// �궨��
#define LOG_FATAL       0     // ���ش���
#define LOG_ERROR       1     // һ�����
#define LOG_WARN        2     // ����
#define LOG_INFO        3     // һ����Ϣ
#define LOG_TRACE       4     // ������Ϣ
#define LOG_DEBUG       5     // ������Ϣ
#define LOG_ALL         6     // ������Ϣ


// ��������
void   WriteLogFile(UINT32 iLogLevel, UINT8 *pszContent);
UINT8 *LogLevel(UINT32 iLogLevel);
void   GetTime(UINT8 *pszTimeStr);

#endif