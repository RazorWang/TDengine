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

#define _DEFAULT_SOURCE
#include "tvariant.h"
#include "ttime.h"
#include "ttokendef.h"
#include "tvariant.h"

#define SET_EXT_INFO(converted, res, minv, maxv, exti)              \
  do {                                                              \
    if (converted == NULL || exti == NULL || *converted == false) { \
      break;                                                        \
    }                                                               \
    if ((res) < (minv)) {                                           \
      *exti = -1;                                                   \
      break;                                                        \
    }                                                               \
    if ((res) > (maxv)) {                                           \
      *exti = 1;                                                    \
      break;                                                        \
    }                                                               \
    assert(0);                                                      \
  } while (0)

int32_t toInteger(const char *z, int32_t n, int32_t base, int64_t *value) {
  errno = 0;
  char *endPtr = NULL;

  *value = taosStr2Int64(z, &endPtr, base);
  if (errno == ERANGE || errno == EINVAL || endPtr - z != n) {
    errno = 0;
    return -1;
  }

  return 0;
}

int32_t toUInteger(const char *z, int32_t n, int32_t base, uint64_t *value) {
  errno = 0;
  char *endPtr = NULL;

  const char *p = z;
  while (*p != 0 && *p == ' ') p++;
  if (*p != 0 && *p == '-') {
    return -1;
  }

  *value = taosStr2UInt64(z, &endPtr, base);
  if (errno == ERANGE || errno == EINVAL || endPtr - z != n) {
    errno = 0;
    return -1;
  }

  return 0;
}

/**
 * create SVariant from binary string, not ascii data
 * @param pVar
 * @param pz
 * @param len
 * @param type
 */
void taosVariantCreateFromBinary(SVariant *pVar, const char *pz, size_t len, uint32_t type) {
  switch (type) {
    case TSDB_DATA_TYPE_BOOL:
    case TSDB_DATA_TYPE_TINYINT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->i = GET_INT8_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_UTINYINT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->u = GET_UINT8_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_SMALLINT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->i = GET_INT16_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_USMALLINT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->u = GET_UINT16_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_INT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->i = GET_INT32_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_UINT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->u = GET_UINT32_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_BIGINT:
    case TSDB_DATA_TYPE_TIMESTAMP: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->i = GET_INT64_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_UBIGINT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->u = GET_UINT64_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_DOUBLE: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->d = GET_DOUBLE_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_FLOAT: {
      pVar->nLen = tDataTypes[type].bytes;
      pVar->d = GET_FLOAT_VAL(pz);
      break;
    }
    case TSDB_DATA_TYPE_NCHAR: {  // here we get the nchar length from raw binary bits length
      size_t lenInwchar = len / TSDB_NCHAR_SIZE;

      pVar->ucs4 = taosMemoryCalloc(1, (lenInwchar + 1) * TSDB_NCHAR_SIZE);
      memcpy(pVar->ucs4, pz, lenInwchar * TSDB_NCHAR_SIZE);
      pVar->nLen = (int32_t)len;

      break;
    }
    case TSDB_DATA_TYPE_BINARY: {  // todo refactor, extract a method
      pVar->pz = taosMemoryCalloc(len + 1, sizeof(char));
      memcpy(pVar->pz, pz, len);
      pVar->nLen = (int32_t)len;
      break;
    }

    default:
      pVar->i = GET_INT32_VAL(pz);
      pVar->nLen = tDataTypes[TSDB_DATA_TYPE_INT].bytes;
  }

  pVar->nType = type;
}

void taosVariantDestroy(SVariant *pVar) {
  if (pVar == NULL) return;

  if (pVar->nType == TSDB_DATA_TYPE_BINARY || pVar->nType == TSDB_DATA_TYPE_NCHAR ||
      pVar->nType == TSDB_DATA_TYPE_JSON) {
    taosMemoryFreeClear(pVar->pz);
    pVar->nLen = 0;
  }

  // NOTE: this is only for string array
  if (pVar->nType == TSDB_DATA_TYPE_POINTER_ARRAY) {
    size_t num = taosArrayGetSize(pVar->arr);
    for (size_t i = 0; i < num; i++) {
      void *p = taosArrayGetP(pVar->arr, i);
      taosMemoryFree(p);
    }
    taosArrayDestroy(pVar->arr);
    pVar->arr = NULL;
  } else if (pVar->nType == TSDB_DATA_TYPE_VALUE_ARRAY) {
    taosArrayDestroy(pVar->arr);
    pVar->arr = NULL;
  }
}

bool taosVariantIsValid(SVariant *pVar) {
  assert(pVar != NULL);
  return isValidDataType(pVar->nType);
}

void taosVariantAssign(SVariant *pDst, const SVariant *pSrc) {
  if (pSrc == NULL || pDst == NULL) return;

  pDst->nType = pSrc->nType;
  if (pSrc->nType == TSDB_DATA_TYPE_BINARY || pSrc->nType == TSDB_DATA_TYPE_NCHAR ||
      pSrc->nType == TSDB_DATA_TYPE_JSON) {
    int32_t len = pSrc->nLen + TSDB_NCHAR_SIZE;
    char   *p = taosMemoryRealloc(pDst->pz, len);
    assert(p);

    memset(p, 0, len);
    pDst->pz = p;

    memcpy(pDst->pz, pSrc->pz, pSrc->nLen);
    pDst->nLen = pSrc->nLen;
    return;
  }

  if (IS_NUMERIC_TYPE(pSrc->nType) || (pSrc->nType == TSDB_DATA_TYPE_BOOL)) {
    pDst->i = pSrc->i;
  } else if (pSrc->nType == TSDB_DATA_TYPE_POINTER_ARRAY) {  // this is only for string array
    size_t num = taosArrayGetSize(pSrc->arr);
    pDst->arr = taosArrayInit(num, sizeof(char *));
    for (size_t i = 0; i < num; i++) {
      char *p = (char *)taosArrayGetP(pSrc->arr, i);
      char *n = strdup(p);
      taosArrayPush(pDst->arr, &n);
    }
  } else if (pSrc->nType == TSDB_DATA_TYPE_VALUE_ARRAY) {
    size_t num = taosArrayGetSize(pSrc->arr);
    pDst->arr = taosArrayInit(num, sizeof(int64_t));
    pDst->nLen = pSrc->nLen;
    assert(pSrc->nLen == num);
    for (size_t i = 0; i < num; i++) {
      int64_t *p = taosArrayGet(pSrc->arr, i);
      taosArrayPush(pDst->arr, p);
    }
  }

  if (pDst->nType != TSDB_DATA_TYPE_POINTER_ARRAY && pDst->nType != TSDB_DATA_TYPE_VALUE_ARRAY) {
    pDst->nLen = tDataTypes[pDst->nType].bytes;
  }
}

int32_t taosVariantCompare(const SVariant *p1, const SVariant *p2) {
  if (p1->nType == TSDB_DATA_TYPE_NULL && p2->nType == TSDB_DATA_TYPE_NULL) {
    return 0;
  }

  if (p1->nType == TSDB_DATA_TYPE_NULL) {
    return -1;
  }

  if (p2->nType == TSDB_DATA_TYPE_NULL) {
    return 1;
  }

  if (p1->nType == TSDB_DATA_TYPE_BINARY || p1->nType == TSDB_DATA_TYPE_NCHAR) {
    if (p1->nLen == p2->nLen) {
      return memcmp(p1->pz, p2->pz, p1->nLen);
    } else {
      return p1->nLen > p2->nLen ? 1 : -1;
    }
  } else if (p1->nType == TSDB_DATA_TYPE_FLOAT || p1->nType == TSDB_DATA_TYPE_DOUBLE) {
    if (p1->d == p2->d) {
      return 0;
    } else {
      return p1->d > p2->d ? 1 : -1;
    }
  } else if (IS_UNSIGNED_NUMERIC_TYPE(p1->nType)) {
    if (p1->u == p2->u) {
      return 0;
    } else {
      return p1->u > p2->u ? 1 : -1;
    }
  } else {
    if (p1->i == p2->i) {
      return 0;
    } else {
      return p1->i > p2->i ? 1 : -1;
    }
  }
}

int32_t taosVariantToString(SVariant *pVar, char *dst) {
  if (pVar == NULL || dst == NULL) return 0;

  switch (pVar->nType) {
    case TSDB_DATA_TYPE_BINARY: {
      int32_t len = sprintf(dst, "\'%s\'", pVar->pz);
      assert(len <= pVar->nLen + sizeof("\'") * 2);  // two more chars
      return len;
    }

    case TSDB_DATA_TYPE_NCHAR: {
      dst[0] = '\'';
      taosUcs4ToMbs(pVar->ucs4, (taosUcs4len(pVar->ucs4) + 1) * TSDB_NCHAR_SIZE, dst + 1);
      int32_t len = (int32_t)strlen(dst);
      dst[len] = '\'';
      dst[len + 1] = 0;
      return len + 1;
    }

    case TSDB_DATA_TYPE_BOOL:
    case TSDB_DATA_TYPE_TINYINT:
    case TSDB_DATA_TYPE_SMALLINT:
    case TSDB_DATA_TYPE_INT:
    case TSDB_DATA_TYPE_UTINYINT:
    case TSDB_DATA_TYPE_USMALLINT:
    case TSDB_DATA_TYPE_UINT:
      return sprintf(dst, "%d", (int32_t)pVar->i);

    case TSDB_DATA_TYPE_BIGINT:
      return sprintf(dst, "%" PRId64, pVar->i);
    case TSDB_DATA_TYPE_UBIGINT:
      return sprintf(dst, "%" PRIu64, pVar->u);
    case TSDB_DATA_TYPE_FLOAT:
    case TSDB_DATA_TYPE_DOUBLE:
      return sprintf(dst, "%.9lf", pVar->d);

    default:
      return 0;
  }
}

static FORCE_INLINE int32_t convertToBoolImpl(char *pStr, int32_t len) {
  if ((strncasecmp(pStr, "true", len) == 0) && (len == 4)) {
    return TSDB_TRUE;
  } else if ((strncasecmp(pStr, "false", len) == 0) && (len == 5)) {
    return TSDB_FALSE;
  } else if (strcasecmp(pStr, TSDB_DATA_NULL_STR_L) == 0) {
    return TSDB_DATA_BOOL_NULL;
  } else {
    return -1;
  }
}

static FORCE_INLINE int32_t wcsconvertToBoolImpl(TdUcs4 *pstr, int32_t len) {
  if ((wcsncasecmp(pstr, L"true", len) == 0) && (len == 4)) {
    return TSDB_TRUE;
  } else if (wcsncasecmp(pstr, L"false", len) == 0 && (len == 5)) {
    return TSDB_FALSE;
  } else if (memcmp(pstr, L"null", wcslen(L"null")) == 0) {
    return TSDB_DATA_BOOL_NULL;
  } else {
    return -1;
  }
}

static int32_t toBinary(SVariant *pVariant, char **pDest, int32_t *pDestSize) {
  const int32_t INITIAL_ALLOC_SIZE = 40;
  char         *pBuf = NULL;

  // it is a in-place convert type for SVariant, local buffer is needed
  if (*pDest == pVariant->pz) {
    pBuf = taosMemoryCalloc(1, INITIAL_ALLOC_SIZE);
  }

  if (pVariant->nType == TSDB_DATA_TYPE_NCHAR) {
    size_t newSize = pVariant->nLen * TSDB_NCHAR_SIZE;
    if (pBuf != NULL) {
      if (newSize >= INITIAL_ALLOC_SIZE) {
        pBuf = taosMemoryRealloc(pBuf, newSize + 1);
      }

      taosUcs4ToMbs(pVariant->ucs4, (int32_t)newSize, pBuf);
      taosMemoryFree(pVariant->ucs4);
      pBuf[newSize] = 0;
    } else {
      taosUcs4ToMbs(pVariant->ucs4, (int32_t)newSize, *pDest);
    }

  } else {
    if (IS_SIGNED_NUMERIC_TYPE(pVariant->nType)) {
      sprintf(pBuf == NULL ? *pDest : pBuf, "%" PRId64, pVariant->i);
    } else if (pVariant->nType == TSDB_DATA_TYPE_DOUBLE || pVariant->nType == TSDB_DATA_TYPE_FLOAT) {
      sprintf(pBuf == NULL ? *pDest : pBuf, "%lf", pVariant->d);
    } else if (pVariant->nType == TSDB_DATA_TYPE_BOOL) {
      sprintf(pBuf == NULL ? *pDest : pBuf, "%s", (pVariant->i == TSDB_TRUE) ? "TRUE" : "FALSE");
    } else if (pVariant->nType == 0) {  // null data
      setNull(pBuf == NULL ? *pDest : pBuf, TSDB_DATA_TYPE_BINARY, 0);
    }
  }

  if (pBuf != NULL) {
    taosMemoryFree(pVariant->pz);
    *pDest = pBuf;
  }

  *pDestSize = (int32_t)strlen(*pDest);
  return 0;
}

static int32_t toNchar(SVariant *pVariant, char **pDest, int32_t *pDestSize) {
  char tmpBuf[40] = {0};

  char   *pDst = tmpBuf;
  int32_t nLen = 0;

  // convert the number to string, than convert it to wchar string.
  if (IS_SIGNED_NUMERIC_TYPE(pVariant->nType)) {
    nLen = sprintf(pDst, "%" PRId64, pVariant->i);
  } else if (IS_UNSIGNED_NUMERIC_TYPE(pVariant->nType)) {
    nLen = sprintf(pDst, "%" PRIu64, pVariant->u);
  } else if (pVariant->nType == TSDB_DATA_TYPE_DOUBLE || pVariant->nType == TSDB_DATA_TYPE_FLOAT) {
    nLen = sprintf(pDst, "%lf", pVariant->d);
  } else if (pVariant->nType == TSDB_DATA_TYPE_BINARY) {
    pDst = pVariant->pz;
    nLen = pVariant->nLen;
  } else if (pVariant->nType == TSDB_DATA_TYPE_BOOL) {
    nLen = sprintf(pDst, "%s", (pVariant->i == TSDB_TRUE) ? "TRUE" : "FALSE");
  }

  if (*pDest == pVariant->pz) {
    TdUcs4 *pWStr = taosMemoryCalloc(1, (nLen + 1) * TSDB_NCHAR_SIZE);
    bool    ret = taosMbsToUcs4(pDst, nLen, pWStr, (nLen + 1) * TSDB_NCHAR_SIZE, NULL);
    if (!ret) {
      taosMemoryFreeClear(pWStr);
      return -1;
    }

    // free the binary buffer in the first place
    if (pVariant->nType == TSDB_DATA_TYPE_BINARY) {
      taosMemoryFree(pVariant->ucs4);
    }

    pVariant->ucs4 = pWStr;
    *pDestSize = taosUcs4len(pVariant->ucs4);

    // shrink the allocate memory, no need to check here.
    char *tmp = taosMemoryRealloc(pVariant->ucs4, (*pDestSize + 1) * TSDB_NCHAR_SIZE);
    assert(tmp != NULL);

    pVariant->ucs4 = (TdUcs4 *)tmp;
  } else {
    int32_t output = 0;

    bool ret = taosMbsToUcs4(pDst, nLen, (TdUcs4 *)*pDest, (nLen + 1) * TSDB_NCHAR_SIZE, &output);
    if (!ret) {
      return -1;
    }

    if (pDestSize != NULL) {
      *pDestSize = output;
    }
  }

  return 0;
}

static FORCE_INLINE int32_t convertToDouble(char *pStr, int32_t len, double *value) {
  //  SToken stoken = {.z = pStr, .n = len};
  //  if (TK_ILLEGAL == tGetNumericStringType(&stoken)) {
  //    return -1;
  //  }
  //
  //  *value = taosStr2Double(pStr, NULL);
  return 0;
}

static FORCE_INLINE int32_t convertToInteger(SVariant *pVariant, int64_t *result, int32_t type, bool issigned,
                                             bool releaseVariantPtr, bool *converted) {
  if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
    setNull((char *)result, type, tDataTypes[type].bytes);
    return 0;
  }

  if (IS_SIGNED_NUMERIC_TYPE(pVariant->nType) || (pVariant->nType == TSDB_DATA_TYPE_BOOL)) {
    *result = pVariant->i;
  } else if (IS_UNSIGNED_NUMERIC_TYPE(pVariant->nType)) {
    *result = pVariant->u;
  } else if (IS_FLOAT_TYPE(pVariant->nType)) {
    *result = (int64_t)pVariant->d;
  } else {
    // TODO: handling var types
  }
#if 0
  errno = 0;
  if (IS_SIGNED_NUMERIC_TYPE(pVariant->nType) || (pVariant->nType == TSDB_DATA_TYPE_BOOL)) {
    *result = pVariant->i;
  } else if (IS_UNSIGNED_NUMERIC_TYPE(pVariant->nType)) {
    *result = pVariant->u;
  } else if (IS_FLOAT_TYPE(pVariant->nType)) {
    *result = (int64_t) pVariant->d;
  } else if (pVariant->nType == TSDB_DATA_TYPE_BINARY) {
    SToken token = {.z = pVariant->pz, .n = pVariant->nLen};
    /*int32_t n = */tGetToken(pVariant->pz, &token.type);

    if (token.type == TK_NULL) {
      if (releaseVariantPtr) {
        taosMemoryFree(pVariant->pz);
        pVariant->nLen = 0;
      }

      setNull((char *)result, type, tDataTypes[type].bytes);
      return 0;
    }

    // decide if it is a valid number
    token.type = tGetNumericStringType(&token);
    if (token.type == TK_ILLEGAL) {
      return -1;
    }

    int64_t res = 0;
    int32_t t = tStrToInteger(token.z, token.type, token.n, &res, issigned);
    if (t != 0) {
      return -1;
    }

    if (releaseVariantPtr) {
      taosMemoryFree(pVariant->pz);
      pVariant->nLen = 0;
    }

    *result = res;
  } else if (pVariant->nType == TSDB_DATA_TYPE_NCHAR) {
    errno = 0;
    TdUcs4 *endPtr = NULL;
    
    SToken token = {0};
    token.n = tGetToken(pVariant->pz, &token.type);
    
    if (token.type == TK_MINUS || token.type == TK_PLUS) {
      token.n = tGetToken(pVariant->pz + token.n, &token.type);
    }
    
    if (token.type == TK_FLOAT) {
      double v = wcstod(pVariant->ucs4, &endPtr);
      if (releaseVariantPtr) {
        taosMemoryFree(pVariant->pz);
        pVariant->nLen = 0;
      }
      
      if ((errno == ERANGE && v == -1) || (isinf(v) || isnan(v))) {
        return -1;
      }
      
      *result = (int64_t)v;
    } else if (token.type == TK_NULL) {
      if (releaseVariantPtr) {
        taosMemoryFree(pVariant->pz);
        pVariant->nLen = 0;
      }
      setNull((char *)result, type, tDataTypes[type].bytes);
      return 0;
    } else {
      int64_t val = wcstoll(pVariant->ucs4, &endPtr, 10);
      if (releaseVariantPtr) {
        taosMemoryFree(pVariant->pz);
        pVariant->nLen = 0;
      }
      
      if (errno == ERANGE) {
        return -1;  // data overflow
      }
      
      *result = val;
    }
  }

  if (converted) {
    *converted = true;
  }

  bool code = false;

  uint64_t ui = 0;
  switch(type) {
    case TSDB_DATA_TYPE_TINYINT:
      code = IS_VALID_TINYINT(*result); break;
    case TSDB_DATA_TYPE_SMALLINT:
      code = IS_VALID_SMALLINT(*result); break;
    case TSDB_DATA_TYPE_INT:
      code = IS_VALID_INT(*result); break;
    case TSDB_DATA_TYPE_BIGINT:
      code = IS_VALID_BIGINT(*result); break;
    case TSDB_DATA_TYPE_UTINYINT:
      ui = *result;
      code = IS_VALID_UTINYINT(ui); break;
    case TSDB_DATA_TYPE_USMALLINT:
      ui = *result;
      code = IS_VALID_USMALLINT(ui); break;
    case TSDB_DATA_TYPE_UINT:
      ui = *result;
      code = IS_VALID_UINT(ui); break;
    case TSDB_DATA_TYPE_UBIGINT:
      ui = *result;
      code = IS_VALID_UBIGINT(ui); break;
  }


  return code? 0:-1;
#endif
  return 0;
}

static int32_t convertToBool(SVariant *pVariant, int64_t *pDest) {
  if (pVariant->nType == TSDB_DATA_TYPE_BOOL) {
    *pDest = pVariant->i;  // in order to be compatible to null of bool
  } else if (IS_NUMERIC_TYPE(pVariant->nType)) {
    *pDest = ((pVariant->i != 0) ? TSDB_TRUE : TSDB_FALSE);
  } else if (pVariant->nType == TSDB_DATA_TYPE_FLOAT || pVariant->nType == TSDB_DATA_TYPE_DOUBLE) {
    *pDest = ((pVariant->d != 0) ? TSDB_TRUE : TSDB_FALSE);
  } else if (pVariant->nType == TSDB_DATA_TYPE_BINARY) {
    int32_t ret = 0;
    if ((ret = convertToBoolImpl(pVariant->pz, pVariant->nLen)) < 0) {
      return ret;
    }

    *pDest = ret;
  } else if (pVariant->nType == TSDB_DATA_TYPE_NCHAR) {
    int32_t ret = 0;
    if ((ret = wcsconvertToBoolImpl(pVariant->ucs4, pVariant->nLen)) < 0) {
      return ret;
    }
    *pDest = ret;
  } else if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
    *pDest = TSDB_DATA_BOOL_NULL;
  }

  assert(*pDest == TSDB_TRUE || *pDest == TSDB_FALSE || *pDest == TSDB_DATA_BOOL_NULL);
  return 0;
}

/*
 * transfer data from variant serve as the implicit data conversion: from input sql string pVariant->nType
 * to column type defined in schema
 */
int32_t tVariantDumpEx(SVariant *pVariant, char *payload, int16_t type, bool includeLengthPrefix, bool *converted,
                       char *extInfo) {
  if (converted) {
    *converted = false;
  }

  if (pVariant == NULL || (pVariant->nType != 0 && !isValidDataType(pVariant->nType))) {
    return -1;
  }

  errno = 0;  // reset global error code
  int64_t result = 0;

  switch (type) {
    case TSDB_DATA_TYPE_BOOL: {
      if (convertToBool(pVariant, &result) < 0) {
        return -1;
      }

      *(int8_t *)payload = (int8_t)result;
      break;
    }

    case TSDB_DATA_TYPE_TINYINT: {
      if (convertToInteger(pVariant, &result, type, true, false, converted) < 0) {
        SET_EXT_INFO(converted, result, INT8_MIN + 1, INT8_MAX, extInfo);
        return -1;
      }
      *((int8_t *)payload) = (int8_t)result;
      break;
    }

    case TSDB_DATA_TYPE_UTINYINT: {
      if (convertToInteger(pVariant, &result, type, false, false, converted) < 0) {
        SET_EXT_INFO(converted, result, 0, UINT8_MAX - 1, extInfo);
        return -1;
      }
      *((uint8_t *)payload) = (uint8_t)result;
      break;
    }

    case TSDB_DATA_TYPE_SMALLINT: {
      if (convertToInteger(pVariant, &result, type, true, false, converted) < 0) {
        SET_EXT_INFO(converted, result, INT16_MIN + 1, INT16_MAX, extInfo);
        return -1;
      }
      *((int16_t *)payload) = (int16_t)result;
      break;
    }

    case TSDB_DATA_TYPE_USMALLINT: {
      if (convertToInteger(pVariant, &result, type, false, false, converted) < 0) {
        SET_EXT_INFO(converted, result, 0, UINT16_MAX - 1, extInfo);
        return -1;
      }
      *((uint16_t *)payload) = (uint16_t)result;
      break;
    }

    case TSDB_DATA_TYPE_INT: {
      if (convertToInteger(pVariant, &result, type, true, false, converted) < 0) {
        SET_EXT_INFO(converted, result, INT32_MIN + 1, INT32_MAX, extInfo);
        return -1;
      }
      *((int32_t *)payload) = (int32_t)result;
      break;
    }

    case TSDB_DATA_TYPE_UINT: {
      if (convertToInteger(pVariant, &result, type, false, false, converted) < 0) {
        SET_EXT_INFO(converted, result, 0, UINT32_MAX - 1, extInfo);
        return -1;
      }
      *((uint32_t *)payload) = (uint32_t)result;
      break;
    }

    case TSDB_DATA_TYPE_BIGINT: {
      if (convertToInteger(pVariant, &result, type, true, false, converted) < 0) {
        SET_EXT_INFO(converted, result, INT64_MIN + 1, INT64_MAX, extInfo);
        return -1;
      }
      *((int64_t *)payload) = (int64_t)result;
      break;
    }

    case TSDB_DATA_TYPE_UBIGINT: {
      if (convertToInteger(pVariant, &result, type, false, false, converted) < 0) {
        SET_EXT_INFO(converted, result, 0, UINT64_MAX - 1, extInfo);
        return -1;
      }
      *((uint64_t *)payload) = (uint64_t)result;
      break;
    }

    case TSDB_DATA_TYPE_FLOAT: {
      if (pVariant->nType == TSDB_DATA_TYPE_BINARY) {
        if (strncasecmp(TSDB_DATA_NULL_STR_L, pVariant->pz, pVariant->nLen) == 0 &&
            strlen(TSDB_DATA_NULL_STR_L) == pVariant->nLen) {
          *((int32_t *)payload) = TSDB_DATA_FLOAT_NULL;
          return 0;
        } else {
          double  value = -1;
          int32_t ret = convertToDouble(pVariant->pz, pVariant->nLen, &value);
          if ((errno == ERANGE && (float)value == -1) || (ret != 0)) {
            return -1;
          }

          if (converted) {
            *converted = true;
          }

          if (value > FLT_MAX || value < -FLT_MAX) {
            SET_EXT_INFO(converted, value, -FLT_MAX, FLT_MAX, extInfo);
            return -1;
          }
          SET_FLOAT_VAL(payload, value);
        }
      } else if (pVariant->nType == TSDB_DATA_TYPE_BOOL || IS_SIGNED_NUMERIC_TYPE(pVariant->nType) ||
                 IS_UNSIGNED_NUMERIC_TYPE(pVariant->nType)) {
        if (converted) {
          *converted = true;
        }

        if (pVariant->i > FLT_MAX || pVariant->i < -FLT_MAX) {
          SET_EXT_INFO(converted, pVariant->i, -FLT_MAX, FLT_MAX, extInfo);
          return -1;
        }

        SET_FLOAT_VAL(payload, pVariant->i);
      } else if (IS_FLOAT_TYPE(pVariant->nType)) {
        if (converted) {
          *converted = true;
        }

        if (pVariant->d > FLT_MAX || pVariant->d < -FLT_MAX) {
          SET_EXT_INFO(converted, pVariant->d, -FLT_MAX, FLT_MAX, extInfo);
          return -1;
        }

        SET_FLOAT_VAL(payload, pVariant->d);
      } else if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
        *((uint32_t *)payload) = TSDB_DATA_FLOAT_NULL;
        return 0;
      }

      float fv = GET_FLOAT_VAL(payload);
      if (isinf(fv) || isnan(fv) || fv > FLT_MAX || fv < -FLT_MAX) {
        return -1;
      }
      break;
    }
    case TSDB_DATA_TYPE_DOUBLE: {
      if (pVariant->nType == TSDB_DATA_TYPE_BINARY) {
        if (strncasecmp(TSDB_DATA_NULL_STR_L, pVariant->pz, pVariant->nLen) == 0 &&
            strlen(TSDB_DATA_NULL_STR_L) == pVariant->nLen) {
          *((int64_t *)payload) = TSDB_DATA_DOUBLE_NULL;
          return 0;
        } else {
          double  value = 0;
          int32_t ret;
          ret = convertToDouble(pVariant->pz, pVariant->nLen, &value);
          if ((errno == ERANGE && value == -1) || (ret != 0)) {
            return -1;
          }

          SET_DOUBLE_VAL(payload, value);
        }
      } else if (pVariant->nType == TSDB_DATA_TYPE_BOOL || IS_SIGNED_NUMERIC_TYPE(pVariant->nType) ||
                 IS_UNSIGNED_NUMERIC_TYPE(pVariant->nType)) {
        SET_DOUBLE_VAL(payload, pVariant->i);
      } else if (IS_FLOAT_TYPE(pVariant->nType)) {
        SET_DOUBLE_VAL(payload, pVariant->d);
      } else if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
        *((int64_t *)payload) = TSDB_DATA_DOUBLE_NULL;
        return 0;
      }

      double dv = GET_DOUBLE_VAL(payload);
      if (errno == ERANGE || isinf(dv) || isnan(dv)) {
        return -1;
      }

      break;
    }

    case TSDB_DATA_TYPE_BINARY: {
      if (!includeLengthPrefix) {
        if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
          *(uint8_t *)payload = TSDB_DATA_BINARY_NULL;
        } else {
          if (pVariant->nType != TSDB_DATA_TYPE_BINARY) {
            toBinary(pVariant, &payload, &pVariant->nLen);
          } else {
            strncpy(payload, pVariant->pz, pVariant->nLen);
          }
        }
      } else {
        if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
          setVardataNull(payload, TSDB_DATA_TYPE_BINARY);
        } else {
          char *p = varDataVal(payload);

          if (pVariant->nType != TSDB_DATA_TYPE_BINARY) {
            toBinary(pVariant, &p, &pVariant->nLen);
          } else {
            strncpy(p, pVariant->pz, pVariant->nLen);
          }

          varDataSetLen(payload, pVariant->nLen);
          assert(p == varDataVal(payload));
        }
      }
      break;
    }
    case TSDB_DATA_TYPE_TIMESTAMP: {
      if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
        *((int64_t *)payload) = TSDB_DATA_BIGINT_NULL;
      } else {
        *((int64_t *)payload) = pVariant->i;
      }
      break;
    }
    case TSDB_DATA_TYPE_NCHAR: {
      int32_t newlen = 0;
      if (!includeLengthPrefix) {
        if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
          *(uint32_t *)payload = TSDB_DATA_NCHAR_NULL;
        } else {
          if (pVariant->nType != TSDB_DATA_TYPE_NCHAR) {
            if (toNchar(pVariant, &payload, &newlen) != 0) {
              return -1;
            }
          } else {
            tasoUcs4Copy((TdUcs4 *)payload, pVariant->ucs4, pVariant->nLen);
          }
        }
      } else {
        if (pVariant->nType == TSDB_DATA_TYPE_NULL) {
          setVardataNull(payload, TSDB_DATA_TYPE_NCHAR);
        } else {
          char *p = varDataVal(payload);

          if (pVariant->nType != TSDB_DATA_TYPE_NCHAR) {
            if (toNchar(pVariant, &p, &newlen) != 0) {
              return -1;
            }
          } else {
            memcpy(p, pVariant->ucs4, pVariant->nLen);
            newlen = pVariant->nLen;
          }

          varDataSetLen(payload, newlen);  // the length may be changed after toNchar function called
          assert(p == varDataVal(payload));
        }
      }

      break;
    }
  }

  return 0;
}

/*
 * transfer data from variant serve as the implicit data conversion: from input sql string pVariant->nType
 * to column type defined in schema
 */
int32_t taosVariantDump(SVariant *pVariant, char *payload, int16_t type, bool includeLengthPrefix) {
  return tVariantDumpEx(pVariant, payload, type, includeLengthPrefix, NULL, NULL);
}

/*
 * In variant, bool/smallint/tinyint/int/bigint share the same attribution of
 * structure, also ignore the convert the type required
 *
 * It is actually the bigint/binary/bool/nchar type transfer
 */
int32_t taosVariantTypeSetType(SVariant *pVariant, char type) {
  if (pVariant == NULL || pVariant->nType == 0) {  // value is not set
    return 0;
  }

  switch (type) {
    case TSDB_DATA_TYPE_BOOL: {  // bool
      if (convertToBool(pVariant, &pVariant->i) < 0) {
        return -1;
      }

      pVariant->nType = type;
      break;
    }
    case TSDB_DATA_TYPE_INT:
    case TSDB_DATA_TYPE_BIGINT:
    case TSDB_DATA_TYPE_TINYINT:
    case TSDB_DATA_TYPE_SMALLINT: {
      convertToInteger(pVariant, &(pVariant->i), type, true, true, NULL);
      pVariant->nType = TSDB_DATA_TYPE_BIGINT;
      break;
    }
    case TSDB_DATA_TYPE_FLOAT:
    case TSDB_DATA_TYPE_DOUBLE: {
      if (pVariant->nType == TSDB_DATA_TYPE_BINARY) {
        errno = 0;
        double v = taosStr2Double(pVariant->pz, NULL);
        if ((errno == ERANGE && v == -1) || (isinf(v) || isnan(v))) {
          taosMemoryFree(pVariant->pz);
          return -1;
        }

        taosMemoryFree(pVariant->pz);
        pVariant->d = v;
      } else if (pVariant->nType == TSDB_DATA_TYPE_NCHAR) {
        errno = 0;
        double v = wcstod(pVariant->ucs4, NULL);
        if ((errno == ERANGE && v == -1) || (isinf(v) || isnan(v))) {
          taosMemoryFree(pVariant->pz);
          return -1;
        }

        taosMemoryFree(pVariant->pz);
        pVariant->d = v;
      } else if (pVariant->nType >= TSDB_DATA_TYPE_BOOL && pVariant->nType <= TSDB_DATA_TYPE_BIGINT) {
        double tmp = (double)pVariant->i;
        pVariant->d = tmp;
      }

      pVariant->nType = TSDB_DATA_TYPE_DOUBLE;
      break;
    }
    case TSDB_DATA_TYPE_BINARY: {
      if (pVariant->nType != TSDB_DATA_TYPE_BINARY) {
        toBinary(pVariant, &pVariant->pz, &pVariant->nLen);
      }
      pVariant->nType = type;
      break;
    }
    case TSDB_DATA_TYPE_NCHAR: {
      if (pVariant->nType != TSDB_DATA_TYPE_NCHAR) {
        if (toNchar(pVariant, &pVariant->pz, &pVariant->nLen) != 0) {
          return -1;
        }
      }
      pVariant->nType = type;
      break;
    }
  }

  return 0;
}

char *taosVariantGet(SVariant *pVar, int32_t type) {
  switch (type) {
    case TSDB_DATA_TYPE_BOOL:
    case TSDB_DATA_TYPE_TINYINT:
    case TSDB_DATA_TYPE_SMALLINT:
    case TSDB_DATA_TYPE_INT:
    case TSDB_DATA_TYPE_BIGINT:
    case TSDB_DATA_TYPE_TIMESTAMP:
      return (char *)&pVar->i;
    case TSDB_DATA_TYPE_UTINYINT:
    case TSDB_DATA_TYPE_USMALLINT:
    case TSDB_DATA_TYPE_UINT:
    case TSDB_DATA_TYPE_UBIGINT:
      return (char *)&pVar->u;
    case TSDB_DATA_TYPE_DOUBLE:
    case TSDB_DATA_TYPE_FLOAT:
      return (char *)&pVar->d;
    case TSDB_DATA_TYPE_BINARY:
    case TSDB_DATA_TYPE_JSON:
      return (char *)pVar->pz;
    case TSDB_DATA_TYPE_NCHAR:
      return (char *)pVar->ucs4;
    default:
      return NULL;
  }

  return NULL;
}
