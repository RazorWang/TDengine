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
#include "tdataformat.h"
#include "tRealloc.h"
#include "tcoding.h"
#include "tdatablock.h"
#include "tlog.h"

// SBuffer ================================
void tBufferDestroy(SBuffer *pBuffer) {
  tFree(pBuffer->pBuf);
  pBuffer->pBuf = NULL;
}

int32_t tBufferInit(SBuffer *pBuffer, int64_t size) {
  pBuffer->nBuf = 0;
  return tRealloc(&pBuffer->pBuf, size);
}

int32_t tBufferPut(SBuffer *pBuffer, const void *pData, int64_t nData) {
  int32_t code = 0;

  code = tRealloc(&pBuffer->pBuf, pBuffer->nBuf + nData);
  if (code) return code;

  memcpy(pBuffer->pBuf + pBuffer->nBuf, pData, nData);
  pBuffer->nBuf += nData;

  return code;
}

// ================================
static int32_t tGetTagVal(uint8_t *p, STagVal *pTagVal, int8_t isJson);

#pragma pack(push, 1)
typedef struct {
  int16_t nCols;
  uint8_t idx[];
} STSKVRow;
#pragma pack(pop)

#define TSROW_IS_KV_ROW(r) ((r)->flags & TSROW_KV_ROW)

// SValue
int32_t tPutValue(uint8_t *p, SValue *pValue, int8_t type) {
  if (IS_VAR_DATA_TYPE(type)) {
    return tPutBinary(p, pValue->pData, pValue->nData);
  } else {
    if (p) memcpy(p, &pValue->val, tDataTypes[type].bytes);
    return tDataTypes[type].bytes;
  }
}

int32_t tGetValue(uint8_t *p, SValue *pValue, int8_t type) {
  if (IS_VAR_DATA_TYPE(type)) {
    return tGetBinary(p, &pValue->pData, pValue ? &pValue->nData : NULL);
  } else {
    memcpy(&pValue->val, p, tDataTypes[type].bytes);
    return tDataTypes[type].bytes;
  }
}

int tValueCmprFn(const SValue *pValue1, const SValue *pValue2, int8_t type) {
  // TODO
  return 0;
}

// STSRow2 ========================================================================
static void setBitMap(uint8_t *pb, uint8_t v, int32_t idx, uint8_t flags) {
  if (pb) {
    switch (flags & 0xf) {
      case TSROW_HAS_NULL | TSROW_HAS_NONE:
      case TSROW_HAS_VAL | TSROW_HAS_NONE:
        if (v) {
          SET_BIT1(pb, idx, (uint8_t)1);
        } else {
          SET_BIT1(pb, idx, (uint8_t)0);
        }
        break;
      case TSROW_HAS_VAL | TSROW_HAS_NULL:
        v = v - 1;
        SET_BIT1(pb, idx, v);
        break;
      case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
        SET_BIT2(pb, idx, v);
        break;

      default:
        ASSERT(0);
    }
  }
}
#define SET_IDX(p, i, n, f)        \
  do {                             \
    if ((f)&TSROW_KV_SMALL) {      \
      ((uint8_t *)(p))[i] = (n);   \
    } else if ((f)&TSROW_KV_MID) { \
      ((uint16_t *)(p))[i] = (n);  \
    } else {                       \
      ((uint32_t *)(p))[i] = (n);  \
    }                              \
  } while (0)

int32_t tTSRowNew(STSRowBuilder *pBuilder, SArray *pArray, STSchema *pTSchema, STSRow2 **ppRow) {
  int32_t code = 0;
#if 0
  STColumn *pTColumn;
  SColVal  *pColVal;
  int32_t   nColVal = taosArrayGetSize(pArray);
  int32_t   iColVal;

  ASSERT(nColVal > 0);

  // try
  uint8_t  flags = 0;
  uint32_t ntv = 0;
  uint32_t nkv = 0;
  int16_t  nTag = 0;
  uint32_t maxIdx = 0;

  iColVal = 0;
  for (int32_t iColumn = 0; iColumn < pTSchema->numOfCols; iColumn++) {
    pTColumn = &pTSchema->columns[iColumn];
    if (iColVal < nColVal) {
      pColVal = (SColVal *)taosArrayGet(pArray, iColVal);
    } else {
      pColVal = NULL;
    }

    if (iColumn == 0) {
      ASSERT(pColVal->cid == pTColumn->colId);
      ASSERT(pTColumn->type == TSDB_DATA_TYPE_TIMESTAMP);
      ASSERT(pTColumn->colId == PRIMARYKEY_TIMESTAMP_COL_ID);

      iColVal++;
    } else {
      if (pColVal) {
        if (pColVal->cid == pTColumn->colId) {
          iColVal++;

          if (COL_VAL_IS_NONE(pColVal)) {
            flags |= TSROW_HAS_NONE;
          } else if (COL_VAL_IS_NULL(pColVal)) {
            flags |= TSROW_HAS_NULL;
            maxIdx = nkv;
            nTag++;
            nkv += tPutI16v(NULL, -pTColumn->colId);
          } else {
            flags |= TSROW_HAS_VAL;
            maxIdx = nkv;
            nTag++;
            nkv += tPutI16v(NULL, pTColumn->colId);
            nkv += tPutValue(NULL, &pColVal->value, pTColumn->type);
            if (IS_VAR_DATA_TYPE(pTColumn->type)) {
              ntv += tPutValue(NULL, &pColVal->value, pTColumn->type);
            }
          }
        } else if (pColVal->cid > pTColumn->colId) {
          flags |= TSROW_HAS_NONE;
        } else {
          ASSERT(0);
        }
      } else {
        flags |= TSROW_HAS_NONE;
      }
    }
  }

  ASSERT(flags);

  // decide
  uint32_t nData = 0;
  uint32_t nDataT = 0;
  uint32_t nDataK = 0;
  if (flags == TSROW_HAS_NONE || flags == TSROW_HAS_NULL) {
    nData = 0;
  } else {
    switch (flags) {
      case TSROW_HAS_VAL:
        nDataT = pTSchema->flen + ntv;
        break;
      case TSROW_HAS_NULL | TSROW_HAS_NONE:
        nDataT = BIT1_SIZE(pTSchema->numOfCols - 1);
        break;
      case TSROW_HAS_VAL | TSROW_HAS_NONE:
      case TSROW_HAS_VAL | TSROW_HAS_NULL:
        nDataT = BIT1_SIZE(pTSchema->numOfCols - 1) + pTSchema->flen + ntv;
        break;
      case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
        nDataT = BIT2_SIZE(pTSchema->numOfCols - 1) + pTSchema->flen + ntv;
        break;
      default:
        break;
        ASSERT(0);
    }

    uint8_t tflags = 0;
    if (maxIdx <= UINT8_MAX) {
      nDataK = sizeof(STSKVRow) + sizeof(uint8_t) * nTag + nkv;
      tflags |= TSROW_KV_SMALL;
    } else if (maxIdx <= UINT16_MAX) {
      nDataK = sizeof(STSKVRow) + sizeof(uint16_t) * nTag + nkv;
      tflags |= TSROW_KV_MID;
    } else {
      nDataK = sizeof(STSKVRow) + sizeof(uint32_t) * nTag + nkv;
      tflags |= TSROW_KV_BIG;
    }

    if (nDataT <= nDataK) {
      nData = nDataT;
    } else {
      nData = nDataK;
      flags |= tflags;
    }
  }

  // alloc
  if (pBuilder) {
    // create from a builder
    if (nData == 0) {
      pBuilder->tsRow.nData = 0;
      pBuilder->tsRow.pData = NULL;
    } else {
      if (pBuilder->szBuf < nData) {
        uint8_t *p = taosMemoryRealloc(pBuilder->pBuf, nData);
        if (p == NULL) {
          code = TSDB_CODE_OUT_OF_MEMORY;
          goto _exit;
        }
        pBuilder->pBuf = p;
        pBuilder->szBuf = nData;
      }

      pBuilder->tsRow.nData = nData;
      pBuilder->tsRow.pData = pBuilder->pBuf;
    }

    *ppRow = &pBuilder->tsRow;
  } else {
    // create a new one
    *ppRow = (STSRow2 *)taosMemoryMalloc(sizeof(STSRow2));
    if (*ppRow == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
    if (nData == 0) {
      (*ppRow)->nData = 0;
      (*ppRow)->pData = NULL;
    } else {
      (*ppRow)->nData = nData;
      (*ppRow)->pData = taosMemoryMalloc(nData);
      if ((*ppRow)->pData == NULL) {
        taosMemoryFree(*ppRow);
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _exit;
      }
    }
  }

  // build
  (*ppRow)->flags = flags;
  (*ppRow)->sver = pTSchema->version;

  pColVal = (SColVal *)taosArrayGet(pArray, 0);
  (*ppRow)->ts = pColVal->value.ts;

  if ((*ppRow)->pData) {
    STSKVRow *pTSKVRow = NULL;
    uint8_t  *pidx = NULL;
    uint8_t  *pkv = NULL;
    uint8_t  *pb = NULL;
    uint8_t  *pf = NULL;
    uint8_t  *ptv = NULL;
    nkv = 0;
    ntv = 0;
    iColVal = 1;

    if ((flags & 0xf0) == 0) {
      switch (flags & 0xf) {
        case TSROW_HAS_VAL:
          pf = (*ppRow)->pData;
          ptv = pf + pTSchema->flen;
          break;
        case TSROW_HAS_NULL | TSROW_HAS_NONE:
          pb = (*ppRow)->pData;
          break;
        case TSROW_HAS_VAL | TSROW_HAS_NONE:
        case TSROW_HAS_VAL | TSROW_HAS_NULL:
          pb = (*ppRow)->pData;
          pf = pb + BIT1_SIZE(pTSchema->numOfCols - 1);
          ptv = pf + pTSchema->flen;
          break;
        case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
          pb = (*ppRow)->pData;
          pf = pb + BIT2_SIZE(pTSchema->numOfCols - 1);
          ptv = pf + pTSchema->flen;
          break;
        default:
          ASSERT(0);
          break;
      }
    } else {
      pTSKVRow = (STSKVRow *)(*ppRow)->pData;
      pTSKVRow->nCols = 0;
      pidx = pTSKVRow->idx;
      if (flags & TSROW_KV_SMALL) {
        pkv = pidx + sizeof(uint8_t) * nTag;
      } else if (flags & TSROW_KV_MID) {
        pkv = pidx + sizeof(uint16_t) * nTag;
      } else {
        pkv = pidx + sizeof(uint32_t) * nTag;
      }
    }

    for (int32_t iColumn = 1; iColumn < pTSchema->numOfCols; iColumn++) {
      pTColumn = &pTSchema->columns[iColumn];
      if (iColVal < nColVal) {
        pColVal = (SColVal *)taosArrayGet(pArray, iColVal);
      } else {
        pColVal = NULL;
      }

      if (pColVal) {
        if (pColVal->cid == pTColumn->colId) {
          iColVal++;

          if (COL_VAL_IS_NONE(pColVal)) {
            goto _set_none;
          } else if (COL_VAL_IS_NULL(pColVal)) {
            goto _set_null;
          } else {
            goto _set_value;
          }
        } else if (pColVal->cid > pTColumn->colId) {
          goto _set_none;
        } else {
          ASSERT(0);
        }
      } else {
        goto _set_none;
      }

    _set_none:
      if ((flags & 0xf0) == 0) {
        setBitMap(pb, 0, iColumn - 1, flags);
        if (flags & TSROW_HAS_VAL) {  // set 0
          if (IS_VAR_DATA_TYPE(pTColumn->type)) {
            *(VarDataOffsetT *)(pf + pTColumn->offset) = 0;
          } else {
            tPutValue(pf + pTColumn->offset, &((SValue){0}), pTColumn->type);
          }
        }
      }
      continue;

    _set_null:
      if ((flags & 0xf0) == 0) {
        setBitMap(pb, 1, iColumn - 1, flags);
        if (flags & TSROW_HAS_VAL) {  // set 0
          if (IS_VAR_DATA_TYPE(pTColumn->type)) {
            *(VarDataOffsetT *)(pf + pTColumn->offset) = 0;
          } else {
            tPutValue(pf + pTColumn->offset, &((SValue){0}), pTColumn->type);
          }
        }
      } else {
        SET_IDX(pidx, pTSKVRow->nCols, nkv, flags);
        pTSKVRow->nCols++;
        nkv += tPutI16v(pkv + nkv, -pTColumn->colId);
      }
      continue;

    _set_value:
      if ((flags & 0xf0) == 0) {
        setBitMap(pb, 2, iColumn - 1, flags);

        if (IS_VAR_DATA_TYPE(pTColumn->type)) {
          *(VarDataOffsetT *)(pf + pTColumn->offset) = ntv;
          ntv += tPutValue(ptv + ntv, &pColVal->value, pTColumn->type);
        } else {
          tPutValue(pf + pTColumn->offset, &pColVal->value, pTColumn->type);
        }
      } else {
        SET_IDX(pidx, pTSKVRow->nCols, nkv, flags);
        pTSKVRow->nCols++;
        nkv += tPutI16v(pkv + nkv, pColVal->cid);
        nkv += tPutValue(pkv + nkv, &pColVal->value, pTColumn->type);
      }
      continue;
    }
  }

#endif
_exit:
  return code;
}

int32_t tTSRowClone(const STSRow2 *pRow, STSRow2 **ppRow) {
  int32_t code = 0;
  int32_t rLen = 0;

  TSROW_LEN(pRow, rLen);
  (*ppRow) = (STSRow2 *)taosMemoryMalloc(rLen);
  if (*ppRow == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  memcpy(*ppRow, pRow, rLen);

_exit:
  return code;
}

void tTSRowFree(STSRow2 *pRow) {
  if (pRow) {
    taosMemoryFree(pRow);
  }
}

void tTSRowGet(STSRow2 *pRow, STSchema *pTSchema, int32_t iCol, SColVal *pColVal) {
#if 0
  uint8_t   isTuple = ((pRow->flags & 0xf0) == 0) ? 1 : 0;
  STColumn *pTColumn = &pTSchema->columns[iCol];
  uint8_t   flags = pRow->flags & (uint8_t)0xf;
  SValue    value;

  ASSERT(iCol < pTSchema->numOfCols);
  ASSERT(flags);
  ASSERT(pRow->sver == pTSchema->version);

  if (iCol == 0) {
    value.ts = pRow->ts;
    goto _return_value;
  }

  if (flags == TSROW_HAS_NONE) {
    goto _return_none;
  } else if (flags == TSROW_HAS_NULL) {
    goto _return_null;
  }

  ASSERT(pRow->nData && pRow->pData);

  if (isTuple) {
    uint8_t *pb = pRow->pData;
    uint8_t *pf = NULL;
    uint8_t *pv = NULL;
    uint8_t *p;
    uint8_t  b;

    // bit
    switch (flags) {
      case TSROW_HAS_VAL:
        pf = pb;
        break;
      case TSROW_HAS_NULL | TSROW_HAS_NONE:
        b = GET_BIT1(pb, iCol - 1);
        if (b == 0) {
          goto _return_none;
        } else {
          goto _return_null;
        }
      case TSROW_HAS_VAL | TSROW_HAS_NONE:
        b = GET_BIT1(pb, iCol - 1);
        if (b == 0) {
          goto _return_none;
        } else {
          pf = pb + BIT1_SIZE(pTSchema->numOfCols - 1);
          break;
        }
      case TSROW_HAS_VAL | TSROW_HAS_NULL:
        b = GET_BIT1(pb, iCol - 1);
        if (b == 0) {
          goto _return_null;
        } else {
          pf = pb + BIT1_SIZE(pTSchema->numOfCols - 1);
          break;
        }
      case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
        b = GET_BIT2(pb, iCol - 1);
        if (b == 0) {
          goto _return_none;
        } else if (b == 1) {
          goto _return_null;
        } else {
          pf = pb + BIT2_SIZE(pTSchema->numOfCols - 1);
          break;
        }
      default:
        ASSERT(0);
    }

    ASSERT(pf);

    p = pf + pTColumn->offset;
    if (IS_VAR_DATA_TYPE(pTColumn->type)) {
      pv = pf + pTSchema->flen;
      p = pv + *(VarDataOffsetT *)p;
    }
    tGetValue(p, &value, pTColumn->type);
    goto _return_value;
  } else {
    STSKVRow *pRowK = (STSKVRow *)pRow->pData;
    int16_t   lidx = 0;
    int16_t   ridx = pRowK->nCols - 1;
    uint8_t  *p;
    int16_t   midx;
    uint32_t  n;
    int16_t   cid;

    ASSERT(pRowK->nCols > 0);

    if (pRow->flags & TSROW_KV_SMALL) {
      p = pRow->pData + sizeof(STSKVRow) + sizeof(uint8_t) * pRowK->nCols;
    } else if (pRow->flags & TSROW_KV_MID) {
      p = pRow->pData + sizeof(STSKVRow) + sizeof(uint16_t) * pRowK->nCols;
    } else if (pRow->flags & TSROW_KV_BIG) {
      p = pRow->pData + sizeof(STSKVRow) + sizeof(uint32_t) * pRowK->nCols;
    } else {
      ASSERT(0);
    }
    while (lidx <= ridx) {
      midx = (lidx + ridx) / 2;

      if (pRow->flags & TSROW_KV_SMALL) {
        n = ((uint8_t *)pRowK->idx)[midx];
      } else if (pRow->flags & TSROW_KV_MID) {
        n = ((uint16_t *)pRowK->idx)[midx];
      } else {
        n = ((uint32_t *)pRowK->idx)[midx];
      }

      n += tGetI16v(p + n, &cid);

      if (TABS(cid) == pTColumn->colId) {
        if (cid < 0) {
          goto _return_null;
        } else {
          n += tGetValue(p + n, &value, pTColumn->type);
          goto _return_value;
        }

        return;
      } else if (TABS(cid) > pTColumn->colId) {
        ridx = midx - 1;
      } else {
        lidx = midx + 1;
      }
    }

    // not found, return NONE
    goto _return_none;
  }

_return_none:
  *pColVal = COL_VAL_NONE(pTColumn->colId, pTColumn->type);
  return;

_return_null:
  *pColVal = COL_VAL_NULL(pTColumn->colId, pTColumn->type);
  return;

_return_value:
  *pColVal = COL_VAL_VALUE(pTColumn->colId, pTColumn->type, value);
  return;
#endif
}

int32_t tTSRowToArray(STSRow2 *pRow, STSchema *pTSchema, SArray **ppArray) {
  int32_t code = 0;
#if 0
  SColVal cv;

  (*ppArray) = taosArrayInit(pTSchema->numOfCols, sizeof(SColVal));
  if (*ppArray == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  for (int32_t iColumn = 0; iColumn < pTSchema->numOfCols; iColumn++) {
    tTSRowGet(pRow, pTSchema, iColumn, &cv);
    taosArrayPush(*ppArray, &cv);
  }

#endif
_exit:
  return code;
}

int32_t tPutTSRow(uint8_t *p, STSRow2 *pRow) {
  int32_t n = 0;

  TSROW_LEN(pRow, n);
  if (p) {
    memcpy(p, pRow, n);
  }

  return n;
}

int32_t tGetTSRow(uint8_t *p, STSRow2 **ppRow) {
  int32_t n = 0;

  *ppRow = (STSRow2 *)p;
  TSROW_LEN(*ppRow, n);

  return n;
}

// STSchema ========================================
int32_t tTSchemaCreate(int32_t sver, SSchema *pSchema, int32_t ncols, STSchema **ppTSchema) {
  *ppTSchema = (STSchema *)taosMemoryMalloc(sizeof(STSchema) + sizeof(STColumn) * ncols);
  if (*ppTSchema == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  (*ppTSchema)->numOfCols = ncols;
  (*ppTSchema)->version = sver;
  (*ppTSchema)->flen = 0;
  (*ppTSchema)->vlen = 0;
  (*ppTSchema)->tlen = 0;

  for (int32_t iCol = 0; iCol < ncols; iCol++) {
    SSchema  *pColumn = &pSchema[iCol];
    STColumn *pTColumn = &((*ppTSchema)->columns[iCol]);

    pTColumn->colId = pColumn->colId;
    pTColumn->type = pColumn->type;
    pTColumn->flags = pColumn->flags;
    pTColumn->bytes = pColumn->bytes;
    pTColumn->offset = (*ppTSchema)->flen;

    // skip first column
    if (iCol) {
      (*ppTSchema)->flen += TYPE_BYTES[pColumn->type];
      if (IS_VAR_DATA_TYPE(pColumn->type)) {
        (*ppTSchema)->vlen += (pColumn->bytes + 5);
      }
    }
  }

  return 0;
}

void tTSchemaDestroy(STSchema *pTSchema) {
  if (pTSchema) taosMemoryFree(pTSchema);
}

// STag ========================================
static int tTagValCmprFn(const void *p1, const void *p2) {
  if (((STagVal *)p1)->cid < ((STagVal *)p2)->cid) {
    return -1;
  } else if (((STagVal *)p1)->cid > ((STagVal *)p2)->cid) {
    return 1;
  }

  return 0;
}
static int tTagValJsonCmprFn(const void *p1, const void *p2) {
  return strcmp(((STagVal *)p1)[0].pKey, ((STagVal *)p2)[0].pKey);
}

static void debugPrintTagVal(int8_t type, const void *val, int32_t vlen, const char *tag, int32_t ln) {
  switch (type) {
    case TSDB_DATA_TYPE_JSON:
    case TSDB_DATA_TYPE_VARCHAR:
    case TSDB_DATA_TYPE_NCHAR: {
      char tmpVal[32] = {0};
      strncpy(tmpVal, val, vlen > 31 ? 31 : vlen);
      printf("%s:%d type:%d vlen:%d, val:\"%s\"\n", tag, ln, (int32_t)type, vlen, tmpVal);
    } break;
    case TSDB_DATA_TYPE_FLOAT:
      printf("%s:%d type:%d vlen:%d, val:%f\n", tag, ln, (int32_t)type, vlen, *(float *)val);
      break;
    case TSDB_DATA_TYPE_DOUBLE:
      printf("%s:%d type:%d vlen:%d, val:%lf\n", tag, ln, (int32_t)type, vlen, *(double *)val);
      break;
    case TSDB_DATA_TYPE_BOOL:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu8 "\n", tag, ln, (int32_t)type, vlen, *(uint8_t *)val);
      break;
    case TSDB_DATA_TYPE_TINYINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi8 "\n", tag, ln, (int32_t)type, vlen, *(int8_t *)val);
      break;
    case TSDB_DATA_TYPE_SMALLINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi16 "\n", tag, ln, (int32_t)type, vlen, *(int16_t *)val);
      break;
    case TSDB_DATA_TYPE_INT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi32 "\n", tag, ln, (int32_t)type, vlen, *(int32_t *)val);
      break;
    case TSDB_DATA_TYPE_BIGINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi64 "\n", tag, ln, (int32_t)type, vlen, *(int64_t *)val);
      break;
    case TSDB_DATA_TYPE_TIMESTAMP:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi64 "\n", tag, ln, (int32_t)type, vlen, *(int64_t *)val);
      break;
    case TSDB_DATA_TYPE_UTINYINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu8 "\n", tag, ln, (int32_t)type, vlen, *(uint8_t *)val);
      break;
    case TSDB_DATA_TYPE_USMALLINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu16 "\n", tag, ln, (int32_t)type, vlen, *(uint16_t *)val);
      break;
    case TSDB_DATA_TYPE_UINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu32 "\n", tag, ln, (int32_t)type, vlen, *(uint32_t *)val);
      break;
    case TSDB_DATA_TYPE_UBIGINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu64 "\n", tag, ln, (int32_t)type, vlen, *(uint64_t *)val);
      break;
    case TSDB_DATA_TYPE_NULL:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi8 "\n", tag, ln, (int32_t)type, vlen, *(int8_t *)val);
      break;
    default:
      ASSERT(0);
      break;
  }
}

void debugPrintSTag(STag *pTag, const char *tag, int32_t ln) {
  int8_t   isJson = pTag->flags & TD_TAG_JSON;
  int8_t   isLarge = pTag->flags & TD_TAG_LARGE;
  uint8_t *p = NULL;
  int16_t  offset = 0;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
  } else {
    p = (uint8_t *)&pTag->idx[pTag->nTag];
  }
  printf("%s:%d >>> STAG === %s:%s, len: %d, nTag: %d, sver:%d\n", tag, ln, isJson ? "json" : "normal",
         isLarge ? "large" : "small", (int32_t)pTag->len, (int32_t)pTag->nTag, pTag->ver);
  for (uint16_t n = 0; n < pTag->nTag; ++n) {
    if (isLarge) {
      offset = ((int16_t *)pTag->idx)[n];
    } else {
      offset = pTag->idx[n];
    }
    STagVal tagVal = {0};
    if (isJson) {
      tagVal.pKey = (char *)POINTER_SHIFT(p, offset);
    } else {
      tagVal.cid = *(int16_t *)POINTER_SHIFT(p, offset);
    }
    printf("%s:%d loop[%d-%d] offset=%d\n", __func__, __LINE__, (int32_t)pTag->nTag, (int32_t)n, (int32_t)offset);
    tGetTagVal(p + offset, &tagVal, isJson);
    if (IS_VAR_DATA_TYPE(tagVal.type)) {
      debugPrintTagVal(tagVal.type, tagVal.pData, tagVal.nData, __func__, __LINE__);
    } else {
      debugPrintTagVal(tagVal.type, &tagVal.i64, tDataTypes[tagVal.type].bytes, __func__, __LINE__);
    }
  }
  printf("\n");
}

static int32_t tPutTagVal(uint8_t *p, STagVal *pTagVal, int8_t isJson) {
  int32_t n = 0;

  // key
  if (isJson) {
    n += tPutCStr(p ? p + n : p, pTagVal->pKey);
  } else {
    n += tPutI16v(p ? p + n : p, pTagVal->cid);
  }

  // type
  n += tPutI8(p ? p + n : p, pTagVal->type);

  // value
  if (IS_VAR_DATA_TYPE(pTagVal->type)) {
    n += tPutBinary(p ? p + n : p, pTagVal->pData, pTagVal->nData);
  } else {
    p = p ? p + n : p;
    n += tDataTypes[pTagVal->type].bytes;
    if (p) memcpy(p, &(pTagVal->i64), tDataTypes[pTagVal->type].bytes);
  }

  return n;
}
static int32_t tGetTagVal(uint8_t *p, STagVal *pTagVal, int8_t isJson) {
  int32_t n = 0;

  // key
  if (isJson) {
    n += tGetCStr(p + n, &pTagVal->pKey);
  } else {
    n += tGetI16v(p + n, &pTagVal->cid);
  }

  // type
  n += tGetI8(p + n, &pTagVal->type);

  // value
  if (IS_VAR_DATA_TYPE(pTagVal->type)) {
    n += tGetBinary(p + n, &pTagVal->pData, &pTagVal->nData);
  } else {
    memcpy(&(pTagVal->i64), p + n, tDataTypes[pTagVal->type].bytes);
    n += tDataTypes[pTagVal->type].bytes;
  }

  return n;
}

bool tTagIsJson(const void *pTag) { return (((const STag *)pTag)->flags & TD_TAG_JSON); }

bool tTagIsJsonNull(void *data) {
  STag  *pTag = (STag *)data;
  int8_t isJson = tTagIsJson(pTag);
  if (!isJson) return false;
  return ((STag *)data)->nTag == 0;
}

int32_t tTagNew(SArray *pArray, int32_t version, int8_t isJson, STag **ppTag) {
  int32_t  code = 0;
  uint8_t *p = NULL;
  int16_t  n = 0;
  int16_t  nTag = taosArrayGetSize(pArray);
  int32_t  szTag = 0;
  int8_t   isLarge = 0;

  // sort
  if (isJson) {
    taosSort(pArray->pData, nTag, sizeof(STagVal), tTagValJsonCmprFn);
  } else {
    taosSort(pArray->pData, nTag, sizeof(STagVal), tTagValCmprFn);
  }

  // get size
  for (int16_t iTag = 0; iTag < nTag; iTag++) {
    szTag += tPutTagVal(NULL, (STagVal *)taosArrayGet(pArray, iTag), isJson);
  }
  if (szTag <= INT8_MAX) {
    szTag = szTag + sizeof(STag) + sizeof(int8_t) * nTag;
  } else {
    szTag = szTag + sizeof(STag) + sizeof(int16_t) * nTag;
    isLarge = 1;
  }

  ASSERT(szTag <= INT16_MAX);

  // build tag
  (*ppTag) = (STag *)taosMemoryCalloc(szTag, 1);
  if ((*ppTag) == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  (*ppTag)->flags = 0;
  if (isJson) {
    (*ppTag)->flags |= TD_TAG_JSON;
  }
  if (isLarge) {
    (*ppTag)->flags |= TD_TAG_LARGE;
  }
  (*ppTag)->len = szTag;
  (*ppTag)->nTag = nTag;
  (*ppTag)->ver = version;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)(*ppTag)->idx)[nTag];
  } else {
    p = (uint8_t *)&(*ppTag)->idx[nTag];
  }
  n = 0;
  for (int16_t iTag = 0; iTag < nTag; iTag++) {
    if (isLarge) {
      ((int16_t *)(*ppTag)->idx)[iTag] = n;
    } else {
      (*ppTag)->idx[iTag] = n;
    }
    n += tPutTagVal(p + n, (STagVal *)taosArrayGet(pArray, iTag), isJson);
  }
#ifdef TD_DEBUG_PRINT_TAG
  debugPrintSTag(*ppTag, __func__, __LINE__);
#endif

  return code;

_err:
  return code;
}

void tTagFree(STag *pTag) {
  if (pTag) taosMemoryFree(pTag);
}

char *tTagValToData(const STagVal *value, bool isJson) {
  if (!value) return NULL;
  char  *data = NULL;
  int8_t typeBytes = 0;
  if (isJson) {
    typeBytes = CHAR_BYTES;
  }
  if (IS_VAR_DATA_TYPE(value->type)) {
    data = taosMemoryCalloc(1, typeBytes + VARSTR_HEADER_SIZE + value->nData);
    if (data == NULL) return NULL;
    if (isJson) *data = value->type;
    varDataLen(data + typeBytes) = value->nData;
    memcpy(varDataVal(data + typeBytes), value->pData, value->nData);
  } else {
    data = ((char *)&(value->i64)) - typeBytes;  // json with type
  }

  return data;
}

bool tTagGet(const STag *pTag, STagVal *pTagVal) {
  if(!pTag || !pTagVal){
    return false;
  }

  int16_t  lidx = 0;
  int16_t  ridx = pTag->nTag - 1;
  int16_t  midx;
  uint8_t *p;
  int8_t   isJson = pTag->flags & TD_TAG_JSON;
  int8_t   isLarge = pTag->flags & TD_TAG_LARGE;
  int16_t  offset;
  STagVal  tv;
  int      c;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
  } else {
    p = (uint8_t *)&pTag->idx[pTag->nTag];
  }

  pTagVal->type = TSDB_DATA_TYPE_NULL;
  pTagVal->pData = NULL;
  pTagVal->nData = 0;
  while (lidx <= ridx) {
    midx = (lidx + ridx) / 2;
    if (isLarge) {
      offset = ((int16_t *)pTag->idx)[midx];
    } else {
      offset = pTag->idx[midx];
    }

    tGetTagVal(p + offset, &tv, isJson);
    if (isJson) {
      c = tTagValJsonCmprFn(pTagVal, &tv);
    } else {
      c = tTagValCmprFn(pTagVal, &tv);
    }

    if (c < 0) {
      ridx = midx - 1;
    } else if (c > 0) {
      lidx = midx + 1;
    } else {
      memcpy(pTagVal, &tv, sizeof(tv));
      return true;
    }
  }
  return false;
}

int32_t tEncodeTag(SEncoder *pEncoder, const STag *pTag) {
  return tEncodeBinary(pEncoder, (const uint8_t *)pTag, pTag->len);
}

int32_t tDecodeTag(SDecoder *pDecoder, STag **ppTag) { return tDecodeBinary(pDecoder, (uint8_t **)ppTag, NULL); }

int32_t tTagToValArray(const STag *pTag, SArray **ppArray) {
  int32_t  code = 0;
  uint8_t *p = NULL;
  STagVal  tv = {0};
  int8_t   isLarge = pTag->flags & TD_TAG_LARGE;
  int16_t  offset = 0;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
  } else {
    p = (uint8_t *)&pTag->idx[pTag->nTag];
  }

  (*ppArray) = taosArrayInit(pTag->nTag + 1, sizeof(STagVal));
  if (*ppArray == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  for (int16_t iTag = 0; iTag < pTag->nTag; iTag++) {
    if (isLarge) {
      offset = ((int16_t *)pTag->idx)[iTag];
    } else {
      offset = pTag->idx[iTag];
    }
    tGetTagVal(p + offset, &tv, pTag->flags & TD_TAG_JSON);
    taosArrayPush(*ppArray, &tv);
  }

  return code;

_err:
  return code;
}

void tTagSetCid(const STag *pTag, int16_t iTag, int16_t cid) {
  uint8_t *p = NULL;
  int8_t   isLarge = pTag->flags & TD_TAG_LARGE;
  int16_t  offset = 0;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
  } else {
    p = (uint8_t *)&pTag->idx[pTag->nTag];
  }

  if (isLarge) {
    offset = ((int16_t *)pTag->idx)[iTag];
  } else {
    offset = pTag->idx[iTag];
  }

  tPutI16v(p + offset, cid);
}

#if 1  // ===================================================================================================================
int tdInitTSchemaBuilder(STSchemaBuilder *pBuilder, schema_ver_t version) {
  if (pBuilder == NULL) return -1;

  pBuilder->tCols = 256;
  pBuilder->columns = (STColumn *)taosMemoryMalloc(sizeof(STColumn) * pBuilder->tCols);
  if (pBuilder->columns == NULL) return -1;

  tdResetTSchemaBuilder(pBuilder, version);
  return 0;
}

void tdDestroyTSchemaBuilder(STSchemaBuilder *pBuilder) {
  if (pBuilder) {
    taosMemoryFreeClear(pBuilder->columns);
  }
}

void tdResetTSchemaBuilder(STSchemaBuilder *pBuilder, schema_ver_t version) {
  pBuilder->nCols = 0;
  pBuilder->tlen = 0;
  pBuilder->flen = 0;
  pBuilder->vlen = 0;
  pBuilder->version = version;
}

int32_t tdAddColToSchema(STSchemaBuilder *pBuilder, int8_t type, int8_t flags, col_id_t colId, col_bytes_t bytes) {
  if (!isValidDataType(type)) return -1;

  if (pBuilder->nCols >= pBuilder->tCols) {
    pBuilder->tCols *= 2;
    STColumn *columns = (STColumn *)taosMemoryRealloc(pBuilder->columns, sizeof(STColumn) * pBuilder->tCols);
    if (columns == NULL) return -1;
    pBuilder->columns = columns;
  }

  STColumn *pCol = &(pBuilder->columns[pBuilder->nCols]);
  pCol->type = type;
  pCol->colId = colId;
  pCol->flags = flags;
  if (pBuilder->nCols == 0) {
    pCol->offset = 0;
  } else {
    STColumn *pTCol = &(pBuilder->columns[pBuilder->nCols - 1]);
    pCol->offset = pTCol->offset + TYPE_BYTES[pTCol->type];
  }

  if (IS_VAR_DATA_TYPE(type)) {
    pCol->bytes = bytes;
    pBuilder->tlen += (TYPE_BYTES[type] + bytes);
    pBuilder->vlen += bytes - sizeof(VarDataLenT);
  } else {
    pCol->bytes = TYPE_BYTES[type];
    pBuilder->tlen += TYPE_BYTES[type];
    pBuilder->vlen += TYPE_BYTES[type];
  }

  pBuilder->nCols++;
  pBuilder->flen += TYPE_BYTES[type];

  ASSERT(pCol->offset < pBuilder->flen);

  return 0;
}

STSchema *tdGetSchemaFromBuilder(STSchemaBuilder *pBuilder) {
  if (pBuilder->nCols <= 0) return NULL;

  int tlen = sizeof(STSchema) + sizeof(STColumn) * pBuilder->nCols;

  STSchema *pSchema = (STSchema *)taosMemoryMalloc(tlen);
  if (pSchema == NULL) return NULL;

  pSchema->version = pBuilder->version;
  pSchema->numOfCols = pBuilder->nCols;
  pSchema->tlen = pBuilder->tlen;
  pSchema->flen = pBuilder->flen;
  pSchema->vlen = pBuilder->vlen;

#ifdef TD_SUPPORT_BITMAP
  pSchema->tlen += (int)TD_BITMAP_BYTES(pSchema->numOfCols);
#endif

  memcpy(&pSchema->columns[0], pBuilder->columns, sizeof(STColumn) * pBuilder->nCols);

  return pSchema;
}

#endif

// SColData ========================================
void tColDataDestroy(void *ph) {
  SColData *pColData = (SColData *)ph;

  tFree(pColData->pBitMap);
  tFree((uint8_t *)pColData->aOffset);
  tFree(pColData->pData);
}

void tColDataInit(SColData *pColData, int16_t cid, int8_t type, int8_t smaOn) {
  pColData->cid = cid;
  pColData->type = type;
  pColData->smaOn = smaOn;
  tColDataClear(pColData);
}

void tColDataClear(SColData *pColData) {
  pColData->nVal = 0;
  pColData->flag = 0;
  pColData->nData = 0;
}

static FORCE_INLINE int32_t tColDataPutValue(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  if (IS_VAR_DATA_TYPE(pColData->type)) {
    code = tRealloc((uint8_t **)(&pColData->aOffset), sizeof(int32_t) * (pColData->nVal + 1));
    if (code) goto _exit;
    pColData->aOffset[pColData->nVal] = pColData->nData;

    if (pColVal->value.nData) {
      code = tRealloc(&pColData->pData, pColData->nData + pColVal->value.nData);
      if (code) goto _exit;
      memcpy(pColData->pData + pColData->nData, pColVal->value.pData, pColVal->value.nData);
      pColData->nData += pColVal->value.nData;
    }
  } else {
    ASSERT(pColData->nData == tDataTypes[pColData->type].bytes * pColData->nVal);
    code = tRealloc(&pColData->pData, pColData->nData + tDataTypes[pColData->type].bytes);
    if (code) goto _exit;
    pColData->nData += tPutValue(pColData->pData + pColData->nData, &pColVal->value, pColVal->type);
  }

_exit:
  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue00(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;
  pColData->flag = HAS_VALUE;
  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;
  pColData->nVal++;
  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue01(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;
  pColData->flag = HAS_NONE;
  pColData->nVal++;
  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue02(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;
  pColData->flag = HAS_NULL;
  pColData->nVal++;
  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue10(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  int32_t nBit = BIT1_SIZE(pColData->nVal + 1);
  code = tRealloc(&pColData->pBitMap, nBit);
  if (code) return code;

  memset(pColData->pBitMap, 0, nBit);
  SET_BIT1(pColData->pBitMap, pColData->nVal, 1);

  pColData->flag |= HAS_VALUE;

  if (pColData->nVal) {
    if (IS_VAR_DATA_TYPE(pColData->type)) {
      int32_t nOffset = sizeof(int32_t) * pColData->nVal;
      code = tRealloc((uint8_t **)(&pColData->aOffset), nOffset);
      if (code) return code;
      memset(pColData->aOffset, 0, nOffset);
    } else {
      pColData->nData = tDataTypes[pColData->type].bytes * pColData->nVal;
      code = tRealloc(&pColData->pData, pColData->nData);
      if (code) return code;
      memset(pColData->pData, 0, pColData->nData);
    }
  }

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue11(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;
  pColData->nVal++;
  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue12(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  int32_t nBit = BIT1_SIZE(pColData->nVal + 1);
  code = tRealloc(&pColData->pBitMap, nBit);
  if (code) return code;

  memset(pColData->pBitMap, 0, nBit);
  SET_BIT1(pColData->pBitMap, pColData->nVal, 1);

  pColData->flag |= HAS_NULL;
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue20(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  int32_t nBit = BIT1_SIZE(pColData->nVal + 1);
  code = tRealloc(&pColData->pBitMap, nBit);
  if (code) return code;

  memset(pColData->pBitMap, 0, nBit);
  SET_BIT1(pColData->pBitMap, pColData->nVal, 1);

  pColData->flag |= HAS_VALUE;

  if (pColData->nVal) {
    if (IS_VAR_DATA_TYPE(pColData->type)) {
      int32_t nOffset = sizeof(int32_t) * pColData->nVal;
      code = tRealloc((uint8_t **)(&pColData->aOffset), nOffset);
      if (code) return code;
      memset(pColData->aOffset, 0, nOffset);
    } else {
      pColData->nData = tDataTypes[pColData->type].bytes * pColData->nVal;
      code = tRealloc(&pColData->pData, pColData->nData);
      if (code) return code;
      memset(pColData->pData, 0, pColData->nData);
    }
  }

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue21(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  int32_t nBit = BIT1_SIZE(pColData->nVal + 1);
  code = tRealloc(&pColData->pBitMap, nBit);
  if (code) return code;

  memset(pColData->pBitMap, 255, nBit);
  SET_BIT1(pColData->pBitMap, pColData->nVal, 0);

  pColData->flag |= HAS_NONE;
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue22(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;
  pColData->nVal++;
  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue30(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  pColData->flag |= HAS_VALUE;

  uint8_t *pBitMap = NULL;
  code = tRealloc(&pBitMap, BIT2_SIZE(pColData->nVal + 1));
  if (code) return code;

  for (int32_t iVal = 0; iVal < pColData->nVal; iVal++) {
    SET_BIT2(pBitMap, iVal, GET_BIT1(pColData->pBitMap, iVal));
  }
  SET_BIT2(pBitMap, pColData->nVal, 2);

  tFree(pColData->pBitMap);
  pColData->pBitMap = pBitMap;

  if (pColData->nVal) {
    if (IS_VAR_DATA_TYPE(pColData->type)) {
      int32_t nOffset = sizeof(int32_t) * pColData->nVal;
      code = tRealloc((uint8_t **)(&pColData->aOffset), nOffset);
      if (code) return code;
      memset(pColData->aOffset, 0, nOffset);
    } else {
      pColData->nData = tDataTypes[pColData->type].bytes * pColData->nVal;
      code = tRealloc(&pColData->pData, pColData->nData);
      if (code) return code;
      memset(pColData->pData, 0, pColData->nData);
    }
  }

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue31(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT1_SIZE(pColData->nVal + 1));
  if (code) return code;

  SET_BIT1(pColData->pBitMap, pColData->nVal, 0);
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue32(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT1_SIZE(pColData->nVal + 1));
  if (code) return code;

  SET_BIT1(pColData->pBitMap, pColData->nVal, 1);
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue40(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;
  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue41(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  pColData->flag |= HAS_NONE;

  int32_t nBit = BIT1_SIZE(pColData->nVal + 1);
  code = tRealloc(&pColData->pBitMap, nBit);
  if (code) return code;

  memset(pColData->pBitMap, 255, nBit);
  SET_BIT1(pColData->pBitMap, pColData->nVal, 0);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue42(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  pColData->flag |= HAS_NULL;

  int32_t nBit = BIT1_SIZE(pColData->nVal + 1);
  code = tRealloc(&pColData->pBitMap, nBit);
  if (code) return code;

  memset(pColData->pBitMap, 255, nBit);
  SET_BIT1(pColData->pBitMap, pColData->nVal, 0);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue50(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT1_SIZE(pColData->nVal + 1));
  if (code) return code;

  SET_BIT1(pColData->pBitMap, pColData->nVal, 1);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue51(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT1_SIZE(pColData->nVal + 1));
  if (code) return code;

  SET_BIT1(pColData->pBitMap, pColData->nVal, 0);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue52(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  pColData->flag |= HAS_NULL;

  uint8_t *pBitMap = NULL;
  code = tRealloc(&pBitMap, BIT2_SIZE(pColData->nVal + 1));
  if (code) return code;

  for (int32_t iVal = 0; iVal < pColData->nVal; iVal++) {
    SET_BIT2(pBitMap, iVal, GET_BIT1(pColData->pBitMap, iVal) ? 2 : 0);
  }
  SET_BIT2(pBitMap, pColData->nVal, 1);

  tFree(pColData->pBitMap);
  pColData->pBitMap = pBitMap;

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue60(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT1_SIZE(pColData->nVal + 1));
  if (code) return code;
  SET_BIT1(pColData->pBitMap, pColData->nVal, 1);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue61(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  pColData->flag |= HAS_NONE;

  uint8_t *pBitMap = NULL;
  code = tRealloc(&pBitMap, BIT2_SIZE(pColData->nVal + 1));
  if (code) return code;

  for (int32_t iVal = 0; iVal < pColData->nVal; iVal++) {
    SET_BIT2(pBitMap, iVal, GET_BIT1(pColData->pBitMap, iVal) ? 2 : 1);
  }
  SET_BIT2(pBitMap, pColData->nVal, 0);

  tFree(pColData->pBitMap);
  pColData->pBitMap = pBitMap;

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue62(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT1_SIZE(pColData->nVal + 1));
  if (code) return code;
  SET_BIT1(pColData->pBitMap, pColData->nVal, 0);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue70(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT2_SIZE(pColData->nVal + 1));
  if (code) return code;
  SET_BIT2(pColData->pBitMap, pColData->nVal, 2);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue71(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT2_SIZE(pColData->nVal + 1));
  if (code) return code;
  SET_BIT2(pColData->pBitMap, pColData->nVal, 0);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static FORCE_INLINE int32_t tColDataAppendValue72(SColData *pColData, SColVal *pColVal) {
  int32_t code = 0;

  code = tRealloc(&pColData->pBitMap, BIT2_SIZE(pColData->nVal + 1));
  if (code) return code;
  SET_BIT2(pColData->pBitMap, pColData->nVal, 1);

  code = tColDataPutValue(pColData, pColVal);
  if (code) return code;

  pColData->nVal++;

  return code;
}
static int32_t (*tColDataAppendValueImpl[8][3])(SColData *pColData, SColVal *pColVal) = {
    {tColDataAppendValue00, tColDataAppendValue01, tColDataAppendValue02},  // 0
    {tColDataAppendValue10, tColDataAppendValue11, tColDataAppendValue12},  // HAS_NONE
    {tColDataAppendValue20, tColDataAppendValue21, tColDataAppendValue22},  // HAS_NULL
    {tColDataAppendValue30, tColDataAppendValue31, tColDataAppendValue32},  // HAS_NULL|HAS_NONE
    {tColDataAppendValue40, tColDataAppendValue41, tColDataAppendValue42},  // HAS_VALUE
    {tColDataAppendValue50, tColDataAppendValue51, tColDataAppendValue52},  // HAS_VALUE|HAS_NONE
    {tColDataAppendValue60, tColDataAppendValue61, tColDataAppendValue62},  // HAS_VALUE|HAS_NULL
    {tColDataAppendValue70, tColDataAppendValue71, tColDataAppendValue72},  // HAS_VALUE|HAS_NULL|HAS_NONE
};
int32_t tColDataAppendValue(SColData *pColData, SColVal *pColVal) {
  ASSERT(pColData->cid == pColVal->cid && pColData->type == pColVal->type);
  return tColDataAppendValueImpl[pColData->flag][pColVal->flag](pColData, pColVal);
}

static FORCE_INLINE void tColDataGetValue1(SColData *pColData, int32_t iVal, SColVal *pColVal) {  // HAS_NONE
  *pColVal = COL_VAL_NONE(pColData->cid, pColData->type);
}
static FORCE_INLINE void tColDataGetValue2(SColData *pColData, int32_t iVal, SColVal *pColVal) {  // HAS_NULL
  *pColVal = COL_VAL_NULL(pColData->cid, pColData->type);
}
static FORCE_INLINE void tColDataGetValue3(SColData *pColData, int32_t iVal, SColVal *pColVal) {  // HAS_NULL|HAS_NONE
  switch (GET_BIT1(pColData->pBitMap, iVal)) {
    case 0:
      *pColVal = COL_VAL_NONE(pColData->cid, pColData->type);
      break;
    case 1:
      *pColVal = COL_VAL_NULL(pColData->cid, pColData->type);
      break;
    default:
      ASSERT(0);
  }
}
static FORCE_INLINE void tColDataGetValue4(SColData *pColData, int32_t iVal, SColVal *pColVal) {  // HAS_VALUE
  SValue value;
  if (IS_VAR_DATA_TYPE(pColData->type)) {
    if (iVal + 1 < pColData->nVal) {
      value.nData = pColData->aOffset[iVal + 1] - pColData->aOffset[iVal];
    } else {
      value.nData = pColData->nData - pColData->aOffset[iVal];
    }
    value.pData = pColData->pData + pColData->aOffset[iVal];
  } else {
    tGetValue(pColData->pData + tDataTypes[pColData->type].bytes * iVal, &value, pColData->type);
  }
  *pColVal = COL_VAL_VALUE(pColData->cid, pColData->type, value);
}
static FORCE_INLINE void tColDataGetValue5(SColData *pColData, int32_t iVal,
                                           SColVal *pColVal) {  // HAS_VALUE|HAS_NONE
  switch (GET_BIT1(pColData->pBitMap, iVal)) {
    case 0:
      *pColVal = COL_VAL_NONE(pColData->cid, pColData->type);
      break;
    case 1:
      tColDataGetValue4(pColData, iVal, pColVal);
      break;
    default:
      ASSERT(0);
  }
}
static FORCE_INLINE void tColDataGetValue6(SColData *pColData, int32_t iVal,
                                           SColVal *pColVal) {  // HAS_VALUE|HAS_NULL
  switch (GET_BIT1(pColData->pBitMap, iVal)) {
    case 0:
      *pColVal = COL_VAL_NULL(pColData->cid, pColData->type);
      break;
    case 1:
      tColDataGetValue4(pColData, iVal, pColVal);
      break;
    default:
      ASSERT(0);
  }
}
static FORCE_INLINE void tColDataGetValue7(SColData *pColData, int32_t iVal,
                                           SColVal *pColVal) {  // HAS_VALUE|HAS_NULL|HAS_NONE
  switch (GET_BIT2(pColData->pBitMap, iVal)) {
    case 0:
      *pColVal = COL_VAL_NONE(pColData->cid, pColData->type);
      break;
    case 1:
      *pColVal = COL_VAL_NULL(pColData->cid, pColData->type);
      break;
    case 2:
      tColDataGetValue4(pColData, iVal, pColVal);
      break;
    default:
      ASSERT(0);
  }
}
static void (*tColDataGetValueImpl[])(SColData *pColData, int32_t iVal, SColVal *pColVal) = {
    NULL,               // 0
    tColDataGetValue1,  // HAS_NONE
    tColDataGetValue2,  // HAS_NULL
    tColDataGetValue3,  // HAS_NULL | HAS_NONE
    tColDataGetValue4,  // HAS_VALUE
    tColDataGetValue5,  // HAS_VALUE | HAS_NONE
    tColDataGetValue6,  // HAS_VALUE | HAS_NULL
    tColDataGetValue7   // HAS_VALUE | HAS_NULL | HAS_NONE
};
void tColDataGetValue(SColData *pColData, int32_t iVal, SColVal *pColVal) {
  ASSERT(iVal >= 0 && iVal < pColData->nVal && pColData->flag);
  tColDataGetValueImpl[pColData->flag](pColData, iVal, pColVal);
}

uint8_t tColDataGetBitValue(SColData *pColData, int32_t iVal) {
  uint8_t v;
  switch (pColData->flag) {
    case HAS_NONE:
      v = 0;
      break;
    case HAS_NULL:
      v = 1;
      break;
    case (HAS_NULL | HAS_NONE):
      v = GET_BIT1(pColData->pBitMap, iVal);
      break;
    case HAS_VALUE:
      v = 2;
      break;
    case (HAS_VALUE | HAS_NONE):
      v = GET_BIT1(pColData->pBitMap, iVal);
      if (v) v = 2;
      break;
    case (HAS_VALUE | HAS_NULL):
      v = GET_BIT1(pColData->pBitMap, iVal) + 1;
      break;
    case (HAS_VALUE | HAS_NULL | HAS_NONE):
      v = GET_BIT2(pColData->pBitMap, iVal);
      break;
    default:
      ASSERT(0);
      break;
  }
  return v;
}

int32_t tColDataCopy(SColData *pColDataSrc, SColData *pColDataDest) {
  int32_t code = 0;
  int32_t size;

  ASSERT(pColDataSrc->nVal > 0);
  ASSERT(pColDataDest->cid == pColDataSrc->cid);
  ASSERT(pColDataDest->type == pColDataSrc->type);

  pColDataDest->smaOn = pColDataSrc->smaOn;
  pColDataDest->nVal = pColDataSrc->nVal;
  pColDataDest->flag = pColDataSrc->flag;

  // bitmap
  if (pColDataSrc->flag != HAS_NONE && pColDataSrc->flag != HAS_NULL && pColDataSrc->flag != HAS_VALUE) {
    size = BIT2_SIZE(pColDataSrc->nVal);
    code = tRealloc(&pColDataDest->pBitMap, size);
    if (code) goto _exit;
    memcpy(pColDataDest->pBitMap, pColDataSrc->pBitMap, size);
  }

  // offset
  if (IS_VAR_DATA_TYPE(pColDataDest->type)) {
    size = sizeof(int32_t) * pColDataSrc->nVal;

    code = tRealloc((uint8_t **)&pColDataDest->aOffset, size);
    if (code) goto _exit;

    memcpy(pColDataDest->aOffset, pColDataSrc->aOffset, size);
  }

  // value
  pColDataDest->nData = pColDataSrc->nData;
  code = tRealloc(&pColDataDest->pData, pColDataSrc->nData);
  if (code) goto _exit;
  memcpy(pColDataDest->pData, pColDataSrc->pData, pColDataDest->nData);

_exit:
  return code;
}
