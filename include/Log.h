#ifndef __DBG_LOG_H__
#define __DBG_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LOG_BUFFER_SIZE 4096
void SetDebugLevel(int level);
int GetDebugLevel(void);
int dbg_log(const char *dbgStr, const char *filePath);
int DBG_signal_safe(const char *dbgStr, const char *filePath);
int DBGLogAndPublish(const char *dbgStr,  const char *path, const char *filename);
int DataLogAndPublish(const char *dbgStr,  const char *path, const char *filename);
void BackupLogFile(const char *origFilePath);
int DBGHandleOldLog(const char *path , const char *filename);

#include <string.h>
#include <stdlib.h>

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

/* ========================================================================= */
/* MODIFIED SECTION: Safe do-while(0) infrastructure wrappers for DBG macros */
/* ========================================================================= */
#define DBG(x, y...) do {\
    if(GetDebugLevel() >= x)\
    {\
        size_t payloadSize = 1 + snprintf(NULL, 0, "[%s:%d %s()]: ", __FILENAME__, __LINE__, __FUNCTION__); \
        size_t dbgSize = 1 + snprintf(NULL, 0, y); \
        size_t totalSize = payloadSize + dbgSize; \
        char *dbgStr = NULL; \
        dbgStr = (char*)malloc(totalSize); \
        if(dbgStr != NULL){ \
            memset(dbgStr, 0, totalSize); \
            snprintf(dbgStr, payloadSize, "[%s:%d %s()]", __FILENAME__, __LINE__, __FUNCTION__); \
            snprintf(dbgStr + strlen(dbgStr), dbgSize, y); \
            DBGLogAndPublish(dbgStr, LOG_STORAGE_LOCATION, DBG_FILE_PATH); \
            free(dbgStr); \
        } \
    }\
} while(0) /* FIXED: Replaced trailing brace with safe do-while structure */

#define DBG_DATA(x, y...) do {\
    if(GetDebugLevel() >= x)\
    {\
        size_t nmeaSize = 1 + snprintf(NULL, 0, y); \
        char *nmeaStr = NULL; \
        nmeaStr = (char*)malloc(nmeaSize); \
        if(nmeaStr != NULL){ \
            memset(nmeaStr, 0, nmeaSize); \
            snprintf(nmeaStr + strlen(nmeaStr), nmeaSize, y); \
            DataLogAndPublish(nmeaStr, LOG_STORAGE_LOCATION, NMEA_FILE_PATH); \
            free(nmeaStr); \
        } \
    }\
} while(0) /* FIXED: Replaced trailing brace with safe do-while structure */
/* ========================================================================= */

#ifdef __cplusplus
}
#endif

#endif