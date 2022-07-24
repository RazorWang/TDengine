###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-

import random
import string
from util.log import *
from util.cases import *
from util.sql import *
from util.common import *
from util.sqlset import *


class TDTestCase:
    def init(self, conn, logSql):
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor())
        self.dbname = 'db_test'
        self.setsql = TDSetSql()
        self.ntbname = f'{self.dbname}.ntb'
        self.rowNum = 10
        self.tbnum = 20
        self.ts = 1537146000000
        self.binary_str = 'taosdata'
        self.nchar_str = '涛思数据'
        self.column_dict = {
            'ts'  : 'timestamp',
            'col1': 'tinyint',
            'col2': 'smallint',
            'col3': 'int',
            'col4': 'bigint',
            'col5': 'tinyint unsigned',
            'col6': 'smallint unsigned',
            'col7': 'int unsigned',
            'col8': 'bigint unsigned',
            'col9': 'float',
            'col10': 'double',
            'col11': 'bool',
            'col12': 'binary(20)',
            'col13': 'nchar(20)'
        }

        self.param_list = [1,100]
    def insert_data(self,column_dict,tbname,row_num):
        insert_sql = self.setsql.set_insertsql(column_dict,tbname,self.binary_str,self.nchar_str)
        for i in range(row_num):
            insert_list = []
            self.setsql.insert_values(column_dict,i,insert_sql,insert_list,self.ts)
    def bottom_check_data(self,tbname,tb_type):
        new_column_dict = {}
        for param in self.param_list:
            for k,v in self.column_dict.items():
                if v.lower() in ['tinyint','smallint','int','bigint','tinyint unsigned','smallint unsigned','int unsigned','bigint unsigned']:
                    tdSql.query(f'select bottom({k},{param}) from {tbname} order by {k}')
                    if param >= self.rowNum:
                        if tb_type in ['normal_table','child_table']:
                            tdSql.checkRows(self.rowNum)
                            values_list = []
                            for i in range(self.rowNum):
                                tp = (i,)
                                values_list.append(tp)
                            tdSql.checkEqual(tdSql.queryResult,values_list)
                        elif tb_type == 'stable':
                            tdSql.checkRows(param)
                    elif param < self.rowNum:
                        if tb_type in ['normal_table','child_table']:
                            tdSql.checkRows(param)
                            values_list = []
                            for i in range(param):
                                tp = (i,)
                                values_list.append(tp)
                            tdSql.checkEqual(tdSql.queryResult,values_list)
                        elif tb_type == 'stable':
                            tdSql.checkRows(param)
                    for i in [self.param_list[0]-1,self.param_list[-1]+1]:
                        tdSql.error(f'select top({k},{i}) from {tbname}')
                    new_column_dict.update({k:v})
                elif v.lower() == 'bool' or 'binary' in v.lower() or 'nchar' in v.lower():
                    tdSql.error(f'select top({k},{param}) from {tbname}')
                tdSql.error(f'select * from {tbname} where top({k},{param})=1')
        pass
    def bottom_check_ntb(self):
        tdSql.execute(f'create database if not exists {self.dbname} vgroups 1')
        tdSql.execute(f'use {self.dbname}')
        tdSql.execute(self.setsql.set_create_normaltable_sql(self.ntbname,self.column_dict))
        self.insert_data(self.column_dict,self.ntbname,self.rowNum)
        self.bottom_check_data(self.ntbname,'normal_table')
        tdSql.execute(f'drop database {self.dbname}')
    def bottom_check_stb(self):
        stbname = f'{self.dbname}.{tdCom.getLongName(5, "letters")}'
        tag_dict = {
            't0':'int'
        }
        tag_values = [
            f'1'
            ]
        tdSql.execute(f"create database if not exists {self.dbname} vgroups 2")
        tdSql.execute(f'use {self.dbname}')
        tdSql.execute(self.setsql.set_create_stable_sql(stbname,self.column_dict,tag_dict))
        for i in range(self.tbnum):
            tdSql.execute(f"create table {stbname}_{i} using {stbname} tags({tag_values[0]})")
            self.insert_data(self.column_dict,f'{stbname}_{i}',self.rowNum)
        tdSql.query(f'show {self.dbname}.tables')
        vgroup_list = []
        for i in range(len(tdSql.queryResult)):
            vgroup_list.append(tdSql.queryResult[i][6])
        vgroup_list_set = set(vgroup_list)
        for i in vgroup_list_set:
            vgroups_num = vgroup_list.count(i)
            if vgroups_num >= 2:
                tdLog.info(f'This scene with {vgroups_num} vgroups is ok!')
            else:
                tdLog.exit(
                    'This scene does not meet the requirements with {vgroups_num} vgroup!\n')
        for i in range(self.tbnum):
            self.bottom_check_data(f'{stbname}_{i}','child_table')
        self.bottom_check_data(f'{stbname}','stable')
        tdSql.execute(f'drop database {self.dbname}')

    def run(self):
        self.bottom_check_ntb()
        self.bottom_check_stb()

    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)

tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())
