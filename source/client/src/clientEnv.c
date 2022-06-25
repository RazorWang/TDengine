/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "catalog.h"
#include "clientInt.h"
#include "clientLog.h"
#include "functionMgt.h"
#include "os.h"
#include "query.h"
#include "scheduler.h"
#include "tcache.h"
#include "tglobal.h"
#include "tmsg.h"
#include "tref.h"
#include "trpc.h"
#include "ttime.h"

#define TSC_VAR_NOT_RELEASE 1
#define TSC_VAR_RELEASED    0

SAppInfo appInfo;
int32_t  clientReqRefPool = -1;
int32_t  clientConnRefPool = -1;

static TdThreadOnce tscinit = PTHREAD_ONCE_INIT;
volatile int32_t    tscInitRes = 0;

static void registerRequest(SRequestObj *pRequest) {
  STscObj *pTscObj = acquireTscObj(*(int64_t *)pRequest->pTscObj->id);

  assert(pTscObj != NULL);

  // connection has been released already, abort creating request.
  pRequest->self = taosAddRef(clientReqRefPool, pRequest);

  int32_t num = atomic_add_fetch_32(&pTscObj->numOfReqs, 1);

  if (pTscObj->pAppInfo) {
    SAppClusterSummary *pSummary = &pTscObj->pAppInfo->summary;

    int32_t total = atomic_add_fetch_64((int64_t *)&pSummary->totalRequests, 1);
    int32_t currentInst = atomic_add_fetch_64((int64_t *)&pSummary->currentRequests, 1);
    tscDebug("0x%" PRIx64 " new Request from connObj:0x%" PRIx64
             ", current:%d, app current:%d, total:%d, reqId:0x%" PRIx64,
             pRequest->self, *(int64_t *)pRequest->pTscObj->id, num, currentInst, total, pRequest->requestId);
  }
}

static void deregisterRequest(SRequestObj *pRequest) {
  assert(pRequest != NULL);

  STscObj *           pTscObj = pRequest->pTscObj;
  SAppClusterSummary *pActivity = &pTscObj->pAppInfo->summary;

  int32_t currentInst = atomic_sub_fetch_64((int64_t *)&pActivity->currentRequests, 1);
  int32_t num = atomic_sub_fetch_32(&pTscObj->numOfReqs, 1);

  int64_t duration = taosGetTimestampUs() - pRequest->metric.start;
  tscDebug("0x%" PRIx64 " free Request from connObj: 0x%" PRIx64 ", reqId:0x%" PRIx64 " elapsed:%" PRIu64
           " ms, current:%d, app current:%d",
           pRequest->self, *(int64_t *)pTscObj->id, pRequest->requestId, duration / 1000, num, currentInst);
  releaseTscObj(*(int64_t *)pTscObj->id);
}

// todo close the transporter properly
void closeTransporter(STscObj *pTscObj) {
  if (pTscObj == NULL || pTscObj->pAppInfo->pTransporter == NULL) {
    return;
  }

  tscDebug("free transporter:%p in connObj: 0x%" PRIx64, pTscObj->pAppInfo->pTransporter, *(int64_t *)pTscObj->id);
  rpcClose(pTscObj->pAppInfo->pTransporter);
}

static bool clientRpcRfp(int32_t code) {
  if (code == TSDB_CODE_RPC_REDIRECT || code == TSDB_CODE_RPC_NETWORK_UNAVAIL || code == TSDB_CODE_NODE_NOT_DEPLOYED ||
      code == TSDB_CODE_SYN_NOT_LEADER || code == TSDB_CODE_APP_NOT_READY) {
    return true;
  } else {
    return false;
  }
}

// TODO refactor
void *openTransporter(const char *user, const char *auth, int32_t numOfThread) {
  SRpcInit rpcInit;
  memset(&rpcInit, 0, sizeof(rpcInit));
  rpcInit.localPort = 0;
  rpcInit.label = "TSC";
  rpcInit.numOfThreads = numOfThread;
  rpcInit.cfp = processMsgFromServer;
  rpcInit.rfp = clientRpcRfp;
  rpcInit.sessions = 1024;
  rpcInit.connType = TAOS_CONN_CLIENT;
  rpcInit.user = (char *)user;
  rpcInit.idleTime = tsShellActivityTimer * 1000;
  void *pDnodeConn = rpcOpen(&rpcInit);
  if (pDnodeConn == NULL) {
    tscError("failed to init connection to server");
    return NULL;
  }

  return pDnodeConn;
}

void closeAllRequests(SHashObj *pRequests) {
  void *pIter = taosHashIterate(pRequests, NULL);
  while (pIter != NULL) {
    int64_t *rid = pIter;

    releaseRequest(*rid);

    pIter = taosHashIterate(pRequests, pIter);
  }
}

void destroyTscObj(void *pObj) {
  STscObj *pTscObj = pObj;

  SClientHbKey connKey = {.tscRid = *(int64_t *)pTscObj->id, .connType = pTscObj->connType};
  hbDeregisterConn(pTscObj->pAppInfo->pAppHbMgr, connKey);
  int64_t connNum = atomic_sub_fetch_64(&pTscObj->pAppInfo->numOfConns, 1);
  closeAllRequests(pTscObj->pRequests);
  schedulerStopQueryHb(pTscObj->pAppInfo->pTransporter);
  if (0 == connNum) {
    // TODO
    closeTransporter(pTscObj);
  }
  tscDebug("connObj 0x%" PRIx64 " destroyed, totalConn:%" PRId64, *(int64_t *)pTscObj->id,
           pTscObj->pAppInfo->numOfConns);
  taosThreadMutexDestroy(&pTscObj->mutex);
  taosMemoryFreeClear(pTscObj);
}

void *createTscObj(const char *user, const char *auth, const char *db, int32_t connType, SAppInstInfo *pAppInfo) {
  STscObj *pObj = (STscObj *)taosMemoryCalloc(1, sizeof(STscObj));
  if (NULL == pObj) {
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return NULL;
  }

  pObj->pRequests = taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, HASH_ENTRY_LOCK);
  if (NULL == pObj->pRequests) {
    taosMemoryFree(pObj);
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return NULL;
  }

  pObj->connType = connType;
  pObj->pAppInfo = pAppInfo;
  tstrncpy(pObj->user, user, sizeof(pObj->user));
  memcpy(pObj->pass, auth, TSDB_PASSWORD_LEN);

  if (db != NULL) {
    tstrncpy(pObj->db, db, tListLen(pObj->db));
  }

  taosThreadMutexInit(&pObj->mutex, NULL);
  pObj->id = taosMemoryMalloc(sizeof(int64_t));
  *(int64_t *)pObj->id = taosAddRef(clientConnRefPool, pObj);
  pObj->schemalessType = 1;

  tscDebug("connObj created, 0x%" PRIx64, *(int64_t *)pObj->id);
  return pObj;
}

STscObj *acquireTscObj(int64_t rid) { return (STscObj *)taosAcquireRef(clientConnRefPool, rid); }

int32_t releaseTscObj(int64_t rid) { return taosReleaseRef(clientConnRefPool, rid); }

void *createRequest(STscObj *pObj, int32_t type) {
  assert(pObj != NULL);

  SRequestObj *pRequest = (SRequestObj *)taosMemoryCalloc(1, sizeof(SRequestObj));
  if (NULL == pRequest) {
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return NULL;
  }

  pRequest->resType = RES_TYPE__QUERY;
  pRequest->pDb = getDbOfConnection(pObj);
  pRequest->requestId = generateRequestId();
  pRequest->metric.start = taosGetTimestampUs();

  pRequest->body.resInfo.convertUcs4 = true;  // convert ucs4 by default

  pRequest->type = type;
  pRequest->pTscObj = pObj;
  pRequest->msgBuf = taosMemoryCalloc(1, ERROR_MSG_BUF_DEFAULT_SIZE);
  pRequest->msgBufLen = ERROR_MSG_BUF_DEFAULT_SIZE;
  tsem_init(&pRequest->body.rspSem, 0, 0);

  registerRequest(pRequest);

  return pRequest;
}

void doFreeReqResultInfo(SReqResultInfo *pResInfo) {
  taosMemoryFreeClear(pResInfo->pRspMsg);
  taosMemoryFreeClear(pResInfo->length);
  taosMemoryFreeClear(pResInfo->row);
  taosMemoryFreeClear(pResInfo->pCol);
  taosMemoryFreeClear(pResInfo->fields);
  taosMemoryFreeClear(pResInfo->userFields);
  taosMemoryFreeClear(pResInfo->convertJson);

  if (pResInfo->convertBuf != NULL) {
    for (int32_t i = 0; i < pResInfo->numOfCols; ++i) {
      taosMemoryFreeClear(pResInfo->convertBuf[i]);
    }
    taosMemoryFreeClear(pResInfo->convertBuf);
  }
}

static void doDestroyRequest(void *p) {
  assert(p != NULL);
  SRequestObj *pRequest = (SRequestObj *)p;

  assert(RID_VALID(pRequest->self));

  taosHashRemove(pRequest->pTscObj->pRequests, &pRequest->self, sizeof(pRequest->self));

  if (pRequest->body.queryJob != 0) {
    schedulerFreeJob(pRequest->body.queryJob, 0);
  }

  taosMemoryFreeClear(pRequest->msgBuf);
  taosMemoryFreeClear(pRequest->sqlstr);
  taosMemoryFreeClear(pRequest->pDb);

  doFreeReqResultInfo(&pRequest->body.resInfo);
  qDestroyQueryPlan(pRequest->body.pDag);

  taosArrayDestroy(pRequest->tableList);
  taosArrayDestroy(pRequest->dbList);

  destroyQueryExecRes(&pRequest->body.resInfo.execRes);

  deregisterRequest(pRequest);
  taosMemoryFreeClear(pRequest);
}

void destroyRequest(SRequestObj *pRequest) {
  if (pRequest == NULL) {
    return;
  }

  taosRemoveRef(clientReqRefPool, pRequest->self);
}

SRequestObj *acquireRequest(int64_t rid) { return (SRequestObj *)taosAcquireRef(clientReqRefPool, rid); }

int32_t releaseRequest(int64_t rid) { return taosReleaseRef(clientReqRefPool, rid); }

void taos_init_imp(void) {
  // In the APIs of other program language, taos_cleanup is not available yet.
  // So, to make sure taos_cleanup will be invoked to clean up the allocated resource to suppress the valgrind warning.
  atexit(taos_cleanup);

  errno = TSDB_CODE_SUCCESS;
  taosSeedRand(taosGetTimestampSec());

  deltaToUtcInitOnce();

  if (taosCreateLog("taoslog", 10, configDir, NULL, NULL, NULL, NULL, 1) != 0) {
    tscInitRes = -1;
    return;
  }

  if (taosInitCfg(configDir, NULL, NULL, NULL, NULL, 1) != 0) {
    tscInitRes = -1;
    return;
  }

  initQueryModuleMsgHandle();

  rpcInit();

  SCatalogCfg cfg = {.maxDBCacheNum = 100, .maxTblCacheNum = 100};
  catalogInit(&cfg);

  SSchedulerCfg scfg = {.maxJobNum = 100};
  schedulerInit(&scfg);
  tscDebug("starting to initialize TAOS driver");

  taosSetCoreDump(true);

  initTaskQueue();
  fmFuncMgtInit();

  clientConnRefPool = taosOpenRef(200, destroyTscObj);
  clientReqRefPool = taosOpenRef(40960, doDestroyRequest);

  // transDestroyBuffer(&conn->readBuf);
  taosGetAppName(appInfo.appName, NULL);
  taosThreadMutexInit(&appInfo.mutex, NULL);

  appInfo.pid = taosGetPId();
  appInfo.startTime = taosGetTimestampMs();
  appInfo.pInstMap = taosHashInit(4, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), true, HASH_ENTRY_LOCK);
  tscDebug("client is initialized successfully");
}

int taos_init() {
  taosThreadOnce(&tscinit, taos_init_imp);
  return tscInitRes;
}

int taos_options_imp(TSDB_OPTION option, const char *str) {
  if (option != TSDB_OPTION_CONFIGDIR) {
    taos_init();  // initialize global config
  } else {
    tstrncpy(configDir, str, PATH_MAX);
    tscInfo("set cfg:%s to %s", configDir, str);
    return 0;
  }

  SConfig *    pCfg = taosGetCfg();
  SConfigItem *pItem = NULL;

  switch (option) {
    case TSDB_OPTION_CONFIGDIR:
      pItem = cfgGetItem(pCfg, "configDir");
      break;
    case TSDB_OPTION_SHELL_ACTIVITY_TIMER:
      pItem = cfgGetItem(pCfg, "shellActivityTimer");
      break;
    case TSDB_OPTION_LOCALE:
      pItem = cfgGetItem(pCfg, "locale");
      break;
    case TSDB_OPTION_CHARSET:
      pItem = cfgGetItem(pCfg, "charset");
      break;
    case TSDB_OPTION_TIMEZONE:
      pItem = cfgGetItem(pCfg, "timezone");
      break;
    default:
      break;
  }

  if (pItem == NULL) {
    tscError("Invalid option %d", option);
    return -1;
  }

  int code = cfgSetItem(pCfg, pItem->name, str, CFG_STYPE_TAOS_OPTIONS);
  if (code != 0) {
    tscError("failed to set cfg:%s to %s since %s", pItem->name, str, terrstr());
  } else {
    tscInfo("set cfg:%s to %s", pItem->name, str);
  }

  return code;
}

/**
 * The request id is an unsigned integer format of 64bit.
 *+------------+-----+-----------+---------------+
 *| uid|localIp| PId | timestamp | serial number |
 *+------------+-----+-----------+---------------+
 *| 12bit      |12bit|24bit      |16bit          |
 *+------------+-----+-----------+---------------+
 * @return
 */
uint64_t generateRequestId() {
  static uint64_t hashId = 0;
  static int32_t  requestSerialId = 0;

  if (hashId == 0) {
    char    uid[64] = {0};
    int32_t code = taosGetSystemUUID(uid, tListLen(uid));
    if (code != TSDB_CODE_SUCCESS) {
      tscError("Failed to get the system uid to generated request id, reason:%s. use ip address instead",
               tstrerror(TAOS_SYSTEM_ERROR(errno)));

    } else {
      hashId = MurmurHash3_32(uid, strlen(uid));
    }
  }

  uint64_t id = 0;

  while (true) {
    int64_t  ts = taosGetTimestampMs();
    uint64_t pid = taosGetPId();
    int32_t  val = atomic_add_fetch_32(&requestSerialId, 1);

    id = ((hashId & 0x0FFF) << 52) | ((pid & 0x0FFF) << 40) | ((ts & 0xFFFFFF) << 16) | (val & 0xFFFF);
    if (id) {
      break;
    }
  }
  return id;
}

#if 0
#include "cJSON.h"
static setConfRet taos_set_config_imp(const char *config){
  setConfRet ret = {SET_CONF_RET_SUCC, {0}};
  static bool setConfFlag = false;
  if (setConfFlag) {
    ret.retCode = SET_CONF_RET_ERR_ONLY_ONCE;
    strcpy(ret.retMsg, "configuration can only set once");
    return ret;
  }
  taosInitGlobalCfg();
  cJSON *root = cJSON_Parse(config);
  if (root == NULL){
    ret.retCode = SET_CONF_RET_ERR_JSON_PARSE;
    strcpy(ret.retMsg, "parse json error");
    return ret;
  }

  int size = cJSON_GetArraySize(root);
  if(!cJSON_IsObject(root) || size == 0) {
    ret.retCode = SET_CONF_RET_ERR_JSON_INVALID;
    strcpy(ret.retMsg, "json content is invalid, must be not empty object");
    return ret;
  }

  if(size >= 1000) {
    ret.retCode = SET_CONF_RET_ERR_TOO_LONG;
    strcpy(ret.retMsg, "json object size is too long");
    return ret;
  }

  for(int i = 0; i < size; i++){
    cJSON *item = cJSON_GetArrayItem(root, i);
    if(!item) {
      ret.retCode = SET_CONF_RET_ERR_INNER;
      strcpy(ret.retMsg, "inner error");
      return ret;
    }
    if(!taosReadConfigOption(item->string, item->valuestring, NULL, NULL, TAOS_CFG_CSTATUS_OPTION, TSDB_CFG_CTYPE_B_CLIENT)){
      ret.retCode = SET_CONF_RET_ERR_PART;
      if (strlen(ret.retMsg) == 0){
        snprintf(ret.retMsg, RET_MSG_LENGTH, "part error|%s", item->string);
      }else{
        int tmp = RET_MSG_LENGTH - 1 - (int)strlen(ret.retMsg);
        size_t leftSize = tmp >= 0 ? tmp : 0;
        strncat(ret.retMsg, "|",  leftSize);
        tmp = RET_MSG_LENGTH - 1 - (int)strlen(ret.retMsg);
        leftSize = tmp >= 0 ? tmp : 0;
        strncat(ret.retMsg, item->string, leftSize);
      }
    }
  }
  cJSON_Delete(root);
  setConfFlag = true;
  return ret;
}

setConfRet taos_set_config(const char *config){
  taosThreadMutexLock(&setConfMutex);
  setConfRet ret = taos_set_config_imp(config);
  taosThreadMutexUnlock(&setConfMutex);
  return ret;
}
#endif
