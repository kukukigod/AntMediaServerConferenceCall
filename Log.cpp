#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <ftw.h>
#include <string>
#include "Log.h"

#define MAX_LOG_FILE_NUM 63
#define SEC_PER_DAY (24 * 3600)
#define MAX_APLOG_RESERVED_SIZE 212 //MB (512 - 50 -250), 50 is a buffer for storage_full issue , 250MB reserved for SyncMgr
#define LOG_INFO_EXTENSION_NAME "logInfo"
#define READY_TO_UPDATE_EXTENSION_NAME ".ReadyToUpload"
#define APP_LOG_PATH "/APLog/"

typedef struct{
    int count;
    int oldestIndex;//Index will be increased when count is larger than MAX_LOG_FILE_NUM
}LogInfo;

static int gDebugLevel = 0;
static pthread_mutex_t gDebugLock = PTHREAD_MUTEX_INITIALIZER;
static size_t gTotalFolderSize = 0;
static unsigned long get_file_size(const char *filePath);
#define DBG_LOG_SIZE_MAX (0x1 << 20)*20 //20MB
#define TIME_LOG_BUFSIZE (64)

void make_log_with_time(std::string &bufCmd, const char *dbgStr)
{
    time_t t;
    char bufTime[TIME_LOG_BUFSIZE] = {0};
    time(&t);
    gmtime_r(&t, reinterpret_cast<struct tm*>(bufTime)); // Use safe parsing or standard text buffer
    struct tm *timeInfo = gmtime(&t);
    strftime(bufTime, TIME_LOG_BUFSIZE, "%b%e %T", timeInfo);
    
    char tmp[MAX_LOG_BUFFER_SIZE] = {0};
    snprintf(tmp, MAX_LOG_BUFFER_SIZE, "[%s] %s", bufTime, dbgStr);
    bufCmd = tmp;
}

static LogInfo ReadLogInfo(const char *origFilePath)
{
    std::string logInfoPath = origFilePath;
    size_t pos = logInfoPath.find_last_of('.');
    
    // Check if extension delimiter exists in the filename part
    size_t slash_pos = logInfoPath.find_last_of('/');
    if (pos == std::string::npos || (slash_pos != std::string::npos && pos < slash_pos)) {
        logInfoPath += ".";
        logInfoPath += LOG_INFO_EXTENSION_NAME;
    } else {
        logInfoPath = logInfoPath.substr(0, pos + 1) + LOG_INFO_EXTENSION_NAME;
    }

    struct stat statbuf;
    LogInfo logInfo;
    int fd;

    //Create log info file if file isn't exist
    if(stat(logInfoPath.c_str(), &statbuf) != 0){
        fd = open(logInfoPath.c_str(), O_CREAT | O_RDWR, 0666);
        logInfo.count = 0;
        logInfo.oldestIndex = 0;
        if (fd >= 0) {
            write(fd, &logInfo, sizeof(LogInfo));
            close(fd);
        }
    }else{
        fd = open(logInfoPath.c_str(), O_RDWR, 0666);
        if (fd >= 0) {
            read(fd, &logInfo, sizeof(LogInfo));
            close(fd);
        }
    }
    return logInfo;
}

static void WriteLogInfo(const char *origFilePath, LogInfo *pLogInfo)
{
    std::string logInfoPath = origFilePath;
    size_t pos = logInfoPath.find_last_of('.');
    
    size_t slash_pos = logInfoPath.find_last_of('/');
    if (pos == std::string::npos || (slash_pos != std::string::npos && pos < slash_pos)) {
        logInfoPath += ".";
        logInfoPath += LOG_INFO_EXTENSION_NAME;
    } else {
        logInfoPath = logInfoPath.substr(0, pos + 1) + LOG_INFO_EXTENSION_NAME;
    }

    int fd = open(logInfoPath.c_str(), O_RDWR, 0666);
    if (fd >= 0) {
        write(fd, pLogInfo, sizeof(LogInfo));
        close(fd);
    }
}

void HandleCurrentApLogFolder(int logFd, const char *folderPath)
{
    //this function need to search all files in the current folder , if the file name incldue the .txt-XX but not .tgz , then compress them to .tgz , and remove the original file

    DIR *pDir = NULL;
    struct dirent *pDirent = NULL;
    std::string log, newPath;

    pDir = opendir(folderPath);
    if(pDir == NULL){
        log = "Failed to open " + std::string(folderPath) + "\n";
        write(logFd, log.c_str(), log.size());
        return;
    }
    while((pDirent = readdir(pDir)) != NULL){
        if(pDirent->d_type == DT_REG){
            newPath = std::string(folderPath) + "/" + pDirent->d_name;
            if(strstr(pDirent->d_name, ".txt-") != NULL && strstr(pDirent->d_name, ".tgz") == NULL){//just compress the file which is txt-XX and not tgz
                log = "Compress " + newPath + "\n";
                write(logFd, log.c_str(), log.size());
                
                //Compress file
                log = "tar zcf " + newPath + ".tgz " + newPath + "\n";
                system(log.c_str());
                write(logFd, log.c_str(), log.size());
                
                //Remove original file
                log = "rm -rf " + newPath + "\n";
                system(log.c_str());
                write(logFd, log.c_str(), log.size());
            }
            else if(get_file_size(newPath.c_str()) > DBG_LOG_SIZE_MAX){//check the pDirent file size , if the size is larger than 20MB , set the gLogNeedToCompress to true , or set it to false
                log = "file " + newPath + " need compress\n";
                write(logFd, log.c_str(), log.size());
                //pthread_mutex_lock(&gDebugLock);
                BackupLogFile(newPath.c_str());
                //pthread_mutex_unlock(&gDebugLock);
            }
        }
    }
    closedir(pDir);
}

void BackupLogFile(const char *origFilePath)
{
    std::string backupFilePath;
    LogInfo logInfo;

    logInfo = ReadLogInfo(origFilePath);
    if(logInfo.count < MAX_LOG_FILE_NUM){
        backupFilePath = std::string(origFilePath) + "-" + std::to_string(logInfo.count);
        rename(origFilePath, backupFilePath.c_str());
        logInfo.count++;
    }else{
        //Remove oldest log file
        backupFilePath = std::string(origFilePath) + "-" + std::to_string(logInfo.oldestIndex);
        unlink(backupFilePath.c_str());
        
        //Rename latest log file
        backupFilePath = std::string(origFilePath) + "-" + std::to_string(logInfo.oldestIndex + logInfo.count);
        rename(origFilePath, backupFilePath.c_str());
        logInfo.oldestIndex++;
    }
    WriteLogInfo(origFilePath, &logInfo);
}

void MarkDebugFile(const char *filename)
{
    std::string newFilename = std::string(filename) + READY_TO_UPDATE_EXTENSION_NAME + "\n";
    // Stripping the newline for actual system call compatibility while keeping original string representation intact
    std::string actualPath = std::string(filename) + READY_TO_UPDATE_EXTENSION_NAME;
    rename(filename, actualPath.c_str());
}

void RemoveDebugFile(const char *filename)
{
    unlink(filename);
}

static void mkdirs(const char *muldir)
{
    int i, len;
    std::string str = muldir;
    len = str.size();
    for(i = 0; i < len; i++){
        if(str[i] == '/'){
            str[i] = '\0';
            if(access(str.c_str(), 0) != 0){
                mkdir(str.c_str(), 0755);
            }
            str[i] = '/';
        }
    }
    if(len > 0 && access(str.c_str(), 0) != 0){
        mkdir(str.c_str(), 0755);
    }
}

static int FtwCallback_Sum(const char *fpath, const struct stat *sb, int typeflag)
{
#if 0
    std::string log = "path = " + std::string(fpath) + ", size = " + std::to_string(sb->st_size) + ", type = " + std::to_string(typeflag) + "\n";
    write(STDERR_FILENO, log.c_str(), log.size());
#endif
    gTotalFolderSize += sb->st_size;
    return 0;
}

static size_t GetTotalFolderSizeMb(const char *path)
{
    size_t totalSize = 0;
    ftw(path, FtwCallback_Sum, 1);
    totalSize = gTotalFolderSize / 1e6;
    gTotalFolderSize = 0;
    return totalSize;
}

static void GetDebugLogPayload(const time_t *t, std::string &timeString)
{
    struct tm sTm;
    const char *moduleName = "";

    if(t != NULL)
        gmtime_r(t, &sTm);
    else{
        time_t tempT;
        tempT = time(NULL);
        gmtime_r(&tempT, &sTm);
    }

    char tmpBuf[128];
    snprintf(tmpBuf, sizeof(tmpBuf), "[%02d:%02d:%02d]<%s>", sTm.tm_hour, sTm.tm_min, sTm.tm_sec, moduleName);
    timeString = tmpBuf;
}

//Older log folder will be compressed. ex: 2022-09-14 -> 2022-09-14.tgz
//Expired log file will be removed
static void HandleOldLog(const char *path, int logFd, time_t t)
{
    time_t tempT;
    DIR *pDir = NULL;
    struct dirent *pDirent = NULL;
    std::string apLogFolder;
    std::string log, timeString;
    std::string cmd;

    GetDebugLogPayload(&t, timeString);
    tempT = t - (t % SEC_PER_DAY);
    apLogFolder = std::string(path) + APP_LOG_PATH;
    
    pDir = opendir(apLogFolder.c_str());
    if(pDir == NULL){
        log = "[" + std::string(__FUNCTION__) + "]Failed to open " + std::string(path) + "\n";
        write(logFd, timeString.c_str(), timeString.size());
        write(logFd, log.c_str(), log.size());
        return;
    }
    while((pDirent = readdir(pDir)) != NULL){
        if(pDirent->d_type == DT_DIR){
            struct tm folderTm;
            time_t folderTime;
            memset(&folderTm, 0, sizeof(struct tm));
            //Check folder format
            if(sscanf(pDirent->d_name, "%d-%d-%d", &folderTm.tm_year, &folderTm.tm_mon, &folderTm.tm_mday) == 3){
                folderTm.tm_year -= 1900;
                folderTm.tm_mon -= 1;
                folderTime = timegm(&folderTm);
                if(tempT > folderTime){
                    //Compressed old logs
                    char tmpLog[512];
                    cmd = "tar zcf " + apLogFolder + pDirent->d_name + ".tgz -C " + apLogFolder + " " + pDirent->d_name + "\n";
                    snprintf(tmpLog, sizeof(tmpLog), "CurT = %ld, FodlerT = %ld, Folder Name = %s\n", (long)tempT, (long)folderTime, pDirent->d_name);
                    log = tmpLog;

                    write(logFd, timeString.c_str(), timeString.size());
                    write(logFd, cmd.c_str(), cmd.size());
                    write(logFd, log.c_str(), log.size());
                    system(cmd.c_str());
                    
                    //Remove old log folder after compressing
                    cmd = "rm -rf " + apLogFolder + pDirent->d_name + "\n";
                    write(logFd, timeString.c_str(), timeString.size());
                    write(logFd, cmd.c_str(), cmd.size());
                    system(cmd.c_str());
                    sync();
                } else if (tempT == folderTime) {
                    std::string curApLogFolder;
                    //check the current folder and compress the .txt-XX file to .tgz
                    curApLogFolder = apLogFolder + pDirent->d_name;
                    HandleCurrentApLogFolder(logFd, curApLogFolder.c_str());
                }
            }
        }
    }
    closedir(pDir);
}

static void RemoveTheOldestLog(const char *path, int logFd)
{
    DIR *pDir = NULL;
    struct dirent *pDirent = NULL;
    std::string log;
    struct tm fileTm, folderTm;
    time_t fileTime, folderTime, currentTime, theOldestTime;
    std::string theOldestPath, timeString;

    currentTime = time(NULL);
    GetDebugLogPayload(&currentTime, timeString);
    currentTime = currentTime - (currentTime % SEC_PER_DAY);
    theOldestTime = currentTime;

    pDir = opendir(path);
    if(pDir == NULL){
        log = "[" + std::string(__FUNCTION__) + "]Failed to open " + std::string(path) + "\n";
        write(logFd, timeString.c_str(), timeString.size());
        write(logFd, log.c_str(), log.size());
        return;
    }
    while((pDirent = readdir(pDir)) != NULL){
        if(pDirent->d_type == DT_DIR){
            memset(&folderTm, 0, sizeof(folderTm));
            if(sscanf(pDirent->d_name, "%d-%d-%d", &folderTm.tm_year, &folderTm.tm_mon, &folderTm.tm_mday) == 3){
                folderTm.tm_year -= 1900;
                folderTm.tm_mon -= 1;
                folderTime = timegm(&folderTm);
                if(theOldestTime > folderTime){
                    theOldestTime = folderTime;
                    theOldestPath = std::string(path) + pDirent->d_name;
                }
            }
        }else if(pDirent->d_type == DT_REG){
            memset(&fileTm, 0, sizeof(fileTm));
            if(sscanf(pDirent->d_name, "%d-%d-%d.tgz", &fileTm.tm_year, &fileTm.tm_mon, &fileTm.tm_mday) == 3){
                fileTm.tm_year -= 1900;
                fileTm.tm_mon -= 1;
                fileTime = timegm(&fileTm);
                if(theOldestTime > fileTime){
                    theOldestTime = fileTime;
                    theOldestPath = std::string(path) + pDirent->d_name;
                }
            }
        }
    }
    if(theOldestTime != currentTime && !theOldestPath.empty()){
        std::string cmd = "rm -rf " + theOldestPath;
        write(logFd, timeString.c_str(), timeString.size());
        write(logFd, cmd.c_str(), cmd.size());
        system(cmd.c_str());
        
        log = "[" + std::string(__FUNCTION__) + "]remove the oldest log path = " + std::string(path) + "\n";
        write(logFd, log.c_str(), log.size());
    }
    closedir(pDir);
    sync();
}

int DBGLogAndPublish(const char *dbgStr,  const char *path, const char *filename)
{
    std::string newPath;
    char payloadLog[MAX_LOG_BUFFER_SIZE] = {0};
    std::string newLog;
    time_t t;
    struct tm sTm;
    const char *moduleName = "";
    struct timeval tv;

    if(dbgStr == NULL)
        return -1;
    pthread_mutex_lock(&gDebugLock);
    gettimeofday(&tv, NULL);
    t = tv.tv_sec;
    gmtime_r(&t, &sTm);
    snprintf(payloadLog, sizeof(payloadLog), "[%02d:%02d:%02d.%lu]<%s>", sTm.tm_hour, sTm.tm_min, sTm.tm_sec, tv.tv_usec / 1000, moduleName);
    newLog = std::string(payloadLog) + std::string(dbgStr);
    fprintf(stderr, "%s", newLog.c_str());
    
    char pathBuf[256];
    snprintf(pathBuf, sizeof(pathBuf), "%s/%s/%d-%02d-%02d/", path, APP_LOG_PATH, sTm.tm_year + 1900, sTm.tm_mon + 1, sTm.tm_mday);
    newPath = std::string(pathBuf);
    mkdirs(newPath.c_str());
    newPath += filename;
    
    int logFd = open(newPath.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if(logFd < 0){
        pthread_mutex_unlock(&gDebugLock);
        return -2;
    }

    write(logFd, newLog.c_str(), newLog.size());
    close(logFd);
    pthread_mutex_unlock(&gDebugLock);

    return 0;
}

int DataLogAndPublish(const char *dbgStr,  const char *path, const char *filename)
{
    std::string newPath;
    std::string newLog;
    time_t t;
    struct tm sTm;
    struct timeval tv;

    if(dbgStr == NULL)
        return -1;
    pthread_mutex_lock(&gDebugLock);
    gettimeofday(&tv, NULL);
    t = tv.tv_sec;
    gmtime_r(&t, &sTm);
    newLog = std::string(dbgStr);
    fprintf(stderr, "%s", newLog.c_str());
    
    char pathBuf[256];
    snprintf(pathBuf, sizeof(pathBuf), "%s/%s/%d-%02d-%02d/", path, APP_LOG_PATH, sTm.tm_year + 1900, sTm.tm_mon + 1, sTm.tm_mday);
    newPath = std::string(pathBuf);
    mkdirs(newPath.c_str());
    newPath += filename;
    
    int logFd = open(newPath.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if(logFd < 0){
        pthread_mutex_unlock(&gDebugLock);
        return -2;
    }

    write(logFd, newLog.c_str(), newLog.size());
    close(logFd);
    pthread_mutex_unlock(&gDebugLock);

    return 0;
}

int DBGHandleOldLog(const char *path , const char *filename)
{
    std::string newPath;
    time_t t;
    struct tm sTm;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    t = tv.tv_sec;
    gmtime_r(&t, &sTm);
    pthread_mutex_lock(&gDebugLock);
    
    char pathBuf[256];
    snprintf(pathBuf, sizeof(pathBuf), "%s/%s/%d-%02d-%02d/", path, APP_LOG_PATH, sTm.tm_year + 1900, sTm.tm_mon + 1, sTm.tm_mday);
    newPath = std::string(pathBuf);
    newPath += filename;
    
    int logFd = open(newPath.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if(logFd < 0){
        pthread_mutex_unlock(&gDebugLock);
        return -2;
    }
    HandleOldLog(path, logFd, t);
    
    //reset newPath to ap_log path
    newPath = std::string(path) + APP_LOG_PATH;
    if(GetTotalFolderSizeMb(newPath.c_str()) > MAX_APLOG_RESERVED_SIZE){
        RemoveTheOldestLog(newPath.c_str(), logFd);
    }
    close(logFd);
    pthread_mutex_unlock(&gDebugLock);
    return 0;
}

int DBG_signal_safe(const char *dbgStr,  const char *filePath)
{
    if (dbgStr == NULL)
       return -1;

    write(STDERR_FILENO, dbgStr, strlen(dbgStr));

    /* neet to make sure this function is signal-safety */

    if (get_file_size(filePath) > DBG_LOG_SIZE_MAX) {
        BackupLogFile(filePath);
    }
    int logFd = open(filePath, O_CREAT | O_RDWR | O_APPEND, S_IRWXU);
    if (logFd < 0)
        return -2;

    write(logFd, dbgStr, strlen(dbgStr));
    close(logFd);
    return 0;
}

int dbg_log(const char *dbgStr, const char *filePath)
{
    while(1){
        if(!access(filePath, F_OK)){
            if(get_file_size(filePath) > DBG_LOG_SIZE_MAX){
                BackupLogFile(filePath);
            }else{
                std::string cmd;
                FILE *pFile = NULL;

                pFile = fopen(filePath, "a+");
                if(pFile == NULL)
                    return -1;

                make_log_with_time(cmd, dbgStr);
                fputs(cmd.c_str(), pFile);
                fclose(pFile);
                break;
            }
        }else{
            std::string cmd = "touch " + std::string(filePath);
            system(cmd.c_str());
        }
    }
    return 0;
}

void SetDebugLevel(int level)
{
    gDebugLevel = level;
}

int GetDebugLevel(void)
{
    return gDebugLevel;
}

/*!
 * @brief get file size
 *
 * @param[in] filePath path of file
 *
 * @return size of file, <0 on fail
 */
static unsigned long get_file_size(const char *fileName)
{
    if (fileName == NULL)
        return -1;

    struct stat buf;
    if (stat(fileName, &buf) != 0)
        return 0;

    return buf.st_size;
}