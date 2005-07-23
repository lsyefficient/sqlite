/*
** 2005 July 8
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code associated with the ANALYZE command.
**
** @(#) $Id: analyze.c,v 1.2 2005/07/23 00:41:49 drh Exp $
*/
#ifndef SQLITE_OMIT_ANALYZE
#include "sqliteInt.h"

/*
** This routine generates code that opens the sqlite_stat1 table on cursor
** iStatCur.
**
** If the sqlite_stat1 tables does not previously exist, it is created.
** If it does previously exist, all entires associated with table zWhere
** are removed.  If zWhere==0 then all entries are removed.
*/
static void openStatTable(
  Parse *pParse,          /* Parsing context */
  int iDb,                /* The database we are looking in */
  int iStatCur,           /* Open the sqlite_stat1 table on this cursor */
  const char *zWhere      /* Delete entries associated with this table */
){
  sqlite3 *db = pParse->db;
  Db *pDb;
  int iRootPage;
  Table *pStat;
  Vdbe *v = sqlite3GetVdbe(pParse);

  pDb = &db->aDb[iDb];
  if( (pStat = sqlite3FindTable(db, "sqlite_stat1", pDb->zName))==0 ){
    /* The sqlite_stat1 tables does not exist.  Create it.  
    ** Note that a side-effect of the CREATE TABLE statement is to leave
    ** the rootpage of the new table on the top of the stack.  This is
    ** important because the OpenWrite opcode below will be needing it. */
    sqlite3NestedParse(pParse,
      "CREATE TABLE %Q.sqlite_stat1(tbl,idx,stat)",
      pDb->zName
    );
    iRootPage = 0;  /* Cause rootpage to be taken from top of stack */
  }else if( zWhere ){
    /* The sqlite_stat1 table exists.  Delete all entries associated with
    ** the table zWhere. */
    sqlite3NestedParse(pParse,
       "DELETE FROM %Q.sqlite_stat1 WHERE tbl=%Q",
       pDb->zName, zWhere
    );
    iRootPage = pStat->tnum;
  }else{
    /* The sqlite_stat1 table already exists.  Delete all rows. */
    iRootPage = pStat->tnum;
    sqlite3VdbeAddOp(v, OP_Clear, pStat->tnum, iDb);
  }

  /* Open the sqlite_stat1 table for writing.
  */
  sqlite3VdbeAddOp(v, OP_Integer, iDb, 0);
  sqlite3VdbeAddOp(v, OP_OpenWrite, iStatCur, 0);
  sqlite3VdbeAddOp(v, OP_SetNumColumns, iStatCur, 3);
}

/*
** Generate code to do an analysis of all indices associated with
** a single table.
*/
static void analyzeOneTable(
  Parse *pParse,   /* Parser context */
  Table *pTab,     /* Table whose indices are to be analyzed */
  int iStatCur,    /* Cursor that writes to the sqlite_stat1 table */
  int iMem         /* Available memory locations begin here */
){
  Index *pIdx;     /* An index to being analyzed */
  int iIdxCur;     /* Cursor number for index being analyzed */
  int nCol;        /* Number of columns in the index */
  Vdbe *v;         /* The virtual machine being built up */
  int i;           /* Loop counter */
  int topOfLoop;   /* The top of the loop */
  int endOfLoop;   /* The end of the loop */
  int addr;        /* The address of an instruction */

  v = sqlite3GetVdbe(pParse);
  if( pTab==0 || pTab->pIndex==0 || pTab->pIndex->pNext==0 ){
    /* Do no analysis for tables with fewer than 2 indices */
    return;
  }
  iIdxCur = pParse->nTab;
  for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
    /* Open a cursor to the index to be analyzed
    */
    sqlite3VdbeAddOp(v, OP_Integer, pIdx->iDb, 0);
    VdbeComment((v, "# %s", pIdx->zName));
    sqlite3VdbeOp3(v, OP_OpenRead, iIdxCur, pIdx->tnum,
                     (char*)&pIdx->keyInfo, P3_KEYINFO);
    nCol = pIdx->nColumn;
    if( iMem+nCol*2>=pParse->nMem ){
      pParse->nMem = iMem+nCol*2+1;
    }
    sqlite3VdbeAddOp(v, OP_SetNumColumns, iIdxCur, nCol+1);

    /* Memory cells are used as follows:
    **
    **    mem[iMem]:             The total number of rows in the table.
    **    mem[iMem+1]:           Number of distinct values in column 1
    **    ...
    **    mem[iMem+nCol]:        Number of distinct values in column N
    **    mem[iMem+nCol+1]       Last observed value of column 1
    **    ...
    **    mem[iMem+nCol+nCol]:   Last observed value of column N
    **
    ** Cells iMem through iMem+nCol are initialized to 0.  The others
    ** are initialized to NULL.
    */
    sqlite3VdbeAddOp(v, OP_Integer, 0, 0);
    for(i=0; i<=nCol; i++){
      sqlite3VdbeAddOp(v, OP_MemStore, iMem+i, i==nCol);
    }
    sqlite3VdbeAddOp(v, OP_Null, 0, 0);
    for(i=0; i<nCol; i++){
      sqlite3VdbeAddOp(v, OP_MemStore, iMem+nCol+i+1, i==nCol-1);
    }

    /* Do the analysis.
    */
    sqlite3VdbeAddOp(v, OP_Rewind, iIdxCur, 0);
    topOfLoop = sqlite3VdbeCurrentAddr(v);
    endOfLoop = sqlite3VdbeMakeLabel(v);
    sqlite3VdbeAddOp(v, OP_MemIncr, iMem, 0);
    for(i=0; i<nCol; i++){
      sqlite3VdbeAddOp(v, OP_Column, iIdxCur, i);
      sqlite3VdbeAddOp(v, OP_MemLoad, iMem+nCol+i+1, 0);
      sqlite3VdbeAddOp(v, OP_Ne, 0x100, 0);
    }
    sqlite3VdbeAddOp(v, OP_Goto, 0, endOfLoop);
    for(i=0; i<nCol; i++){
      addr = sqlite3VdbeAddOp(v, OP_MemIncr, iMem+i+1, 0);
      sqlite3VdbeChangeP2(v, topOfLoop + 3*i + 3, addr);
      sqlite3VdbeAddOp(v, OP_Column, iIdxCur, i);
      sqlite3VdbeAddOp(v, OP_MemStore, iMem+nCol+i+1, 1);
    }
    sqlite3VdbeResolveLabel(v, endOfLoop);
    sqlite3VdbeAddOp(v, OP_Next, iIdxCur, topOfLoop);
    sqlite3VdbeAddOp(v, OP_Close, iIdxCur, 0);

    /* Store the results.  
    **
    ** The result is a single row of the sqlite_stmt1 table.  The first
    ** two columns are the names of the table and index.  The third column
    ** is a string composed of a list of integer statistics about the
    ** index.  There is one integer in the list for each column of the table.
    ** This integer is a guess of how many rows of the table the index will
    ** select.  If D is the count of distinct values and K is the total
    ** number of rows, then the integer is computed as:
    **
    **        I = (K+D-1)/D
    **
    ** If K==0 then no entry is made into the sqlite_stat1 table.  
    ** If K>0 then it is always the case the D>0 so division by zero
    ** is never possible.
    */
    sqlite3VdbeAddOp(v, OP_MemLoad, iMem, 0);
    addr = sqlite3VdbeAddOp(v, OP_IfNot, 0, 0);
    sqlite3VdbeAddOp(v, OP_NewRowid, iStatCur, 0);
    sqlite3VdbeOp3(v, OP_String8, 0, 0, pTab->zName, 0);
    sqlite3VdbeOp3(v, OP_String8, 0, 0, pIdx->zName, 0);
    for(i=0; i<nCol; i++){
      sqlite3VdbeAddOp(v, OP_MemLoad, iMem, 0);
      sqlite3VdbeAddOp(v, OP_MemLoad, iMem+i+1, 0);
      sqlite3VdbeAddOp(v, OP_Add, 0, 0);
      sqlite3VdbeAddOp(v, OP_AddImm, -1, 0);
      sqlite3VdbeAddOp(v, OP_MemLoad, iMem+i+1, 0);
      sqlite3VdbeAddOp(v, OP_Divide, 0, 0);
      if( i==nCol-1 ){
        if( i>0 ){
          sqlite3VdbeAddOp(v, OP_Concat, nCol*2-3, 0);
        }
      }else{
        if( i==0 ){
          sqlite3VdbeOp3(v, OP_String8, 0, 0, " ", 0);
        }else{
          sqlite3VdbeAddOp(v, OP_Dup, 1, 0);
        }
      }
    }
    sqlite3VdbeOp3(v, OP_MakeRecord, 3, 0, "ttt", 0);
    sqlite3VdbeAddOp(v, OP_Insert, iStatCur, 0);
    sqlite3VdbeChangeP2(v, addr, sqlite3VdbeCurrentAddr(v));
  }
}

/*
** Generate code that will do an analysis of an entire database
*/
static void analyzeDatabase(Parse *pParse, int iDb){
  sqlite3 *db = pParse->db;
  HashElem *k;
  int iStatCur;
  int iMem;

  sqlite3BeginWriteOperation(pParse, 0, iDb);
  iStatCur = pParse->nTab++;
  openStatTable(pParse, iDb, iStatCur, 0);
  iMem = pParse->nMem;
  for(k=sqliteHashFirst(&db->aDb[iDb].tblHash);  k; k=sqliteHashNext(k)){
    Table *pTab = (Table*)sqliteHashData(k);
    analyzeOneTable(pParse, pTab, iStatCur, iMem);
  }
}

/*
** Generate code that will do an analysis of a single table in
** a database.
*/
static void analyzeTable(Parse *pParse, Table *pTab){
  int iDb;
  int iStatCur;

  assert( pTab!=0 );
  iDb = pTab->iDb;
  sqlite3BeginWriteOperation(pParse, 0, iDb);
  iStatCur = pParse->nTab++;
  openStatTable(pParse, iDb, iStatCur, pTab->zName);
  analyzeOneTable(pParse, pTab, iStatCur, pParse->nMem);
}

/*
** Generate code for the ANALYZE command.  The parser calls this routine
** when it recognizes an ANALYZE command.
**
**        ANALYZE                            -- 1
**        ANALYZE  <database>                -- 2
**        ANALYZE  ?<database>.?<tablename>  -- 3
**
** Form 1 causes all indices in all attached databases to be analyzed.
** Form 2 analyzes all indices the single database named.
** Form 3 analyzes all indices associated with the named table.
*/
void sqlite3Analyze(Parse *pParse, Token *pName1, Token *pName2){
  sqlite3 *db = pParse->db;
  int iDb;
  int i;
  char *z, *zDb;
  Table *pTab;
  Token *pTableName;

  /* Read the database schema. If an error occurs, leave an error message
  ** and code in pParse and return NULL. */
  if( SQLITE_OK!=sqlite3ReadSchema(pParse) ){
    return;
  }

  if( pName1==0 ){
    /* Form 1:  Analyze everything */
    for(i=0; i<db->nDb; i++){
      if( i==1 ) continue;  /* Do not analyze the TEMP database */
      analyzeDatabase(pParse, i);
    }
  }else if( pName2==0 ){
    /* Form 2:  Analyze the database or table named */
    iDb = sqlite3FindDb(db, pName1);
    if( iDb>=0 ){
      analyzeDatabase(pParse, iDb);
      return;
    }
    z = sqlite3NameFromToken(pName1);
    pTab = sqlite3LocateTable(pParse, z, 0);
    sqliteFree(z);
    if( pTab ){
      analyzeTable(pParse, pTab);
    }
    return;
  }else{
    /* Form 3: Analyze the fully qualified table name */
    iDb = sqlite3TwoPartName(pParse, pName1, pName2, &pTableName);
    if( iDb>=0 ){
      zDb = db->aDb[iDb].zName;
      z = sqlite3NameFromToken(pTableName);
      pTab = sqlite3LocateTable(pParse, z, zDb);
      sqliteFree(z);
      if( pTab ){
        analyzeTable(pParse, pTab);
      }
    }   
  }
}



#endif /* SQLITE_OMIT_ANALYZE */
