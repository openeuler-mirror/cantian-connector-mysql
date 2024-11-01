# Copyright (C) 2023. Huawei Technologies Co., Ltd. All rights reserved.

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.

#  You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA 
SET(CTC_PLUGIN_DYNAMIC "ha_ctc")
SET(CTC_SOURCES ctc_log.h ha_ctc.cc ha_ctc.h ha_ctcpart.h ha_ctcpart.cc ha_ctc_ddl.cc ha_ctc_ddl.h ctc_ddl_util.cc ctc_ddl_util.h datatype_cnvrtr.cc datatype_cnvrtr.h ctc_error.cc ctc_error.h
                decimal_convert.cc decimal_convert.h ctc_cbo.cc ctc_cbo.h datatype_cnvrt_4_index_search.cc datatype_cnvrt_4_index_search.h ctc_ddl_rewriter_plugin.cc
                ctc_meta_data.cc ctc_meta_data.h ctc_stats.h ctc_stats.cc ctc_srv.h ctc_util.h ctc_util.cc protobuf/tc_db.pb-c.c protobuf/tc_db.pb-c.h)
option(WITH_DAAC OFF)
IF (WITH_DAAC)
    add_definitions(-DWITH_DAAC)
    message(STATUS "ADD WITH DAAC")
    list(APPEND CTC_SOURCES mysql_daac_plugin.cc)
ELSE ()
    list(APPEND CTC_SOURCES ctc_srv_mq_stub.cc ctc_srv_mq_module.cc ctc_srv_mq_module.h srv_mq_msg.h message_queue/dsw_shm.h 
                            message_queue/dsw_list.h message_queue/dsw_message.h message_queue/dsw_typedef.h)
ENDIF ()
ADD_DEFINITIONS(-DMYSQL_SERVER)
ADD_DEFINITIONS(-DMYSQL_DYNAMIC_PLUGIN)
STRING_APPEND(CMAKE_CXX_FLAGS " -Wno-implicit-fallthrough")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wno-stringop-overflow")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wunused-variable")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wunused-parameter")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wunused-but-set-variable")
# STRING_APPEND(CMAKE_CXX_FLAGS " -Wstringop-truncation")

add_compile_options(-fstack-protector)
add_compile_options(-fvisibility=hidden)
SET(CMAKE_SKIP_RPATH TRUE)

STRING_APPEND(CMAKE_CXX_FLAGS " -Wl,-z,now")
STRING_APPEND(CMAKE_CXX_FLAGS " -Wl,-z,nodelete")
SET(DAAC_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../daac_lib)
SET(MYSQL_CLIENT_STATIC_LIB_PATH ${CMAKE_BINARY_DIR}/archive_output_directory/)
LINK_DIRECTORIES(${DAAC_LIB_PATH})
LINK_DIRECTORIES(${MYSQL_CLIENT_STATIC_LIB_PATH})
INCLUDE_DIRECTORIES(SYSTEM ${BOOST_PATCHES_DIR} ${BOOST_INCLUDE_DIR})

SET(CTC_PROXY_SOURCES ctc_proxy_util.cc ctc_proxy_util.h ctc_mysql_proxy.cc)
SET(CTC_PROXY_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-Bsymbolic")
add_library(ctc_proxy SHARED ${CTC_PROXY_SOURCES})
target_link_libraries(ctc_proxy libmysqlclient.a)
# ctc_proxy link libmysqlclient.a function symbol rather than mysqld`s
set_target_properties(ctc_proxy PROPERTIES COMPILE_FLAGS ${CTC_PROXY_CXX_FLAGS} LINK_FLAGS " -Wl,-Bsymbolic"
    LIBRARY_OUTPUT_DIRECTORY ${DAAC_LIB_PATH})
IF (WITH_DAAC)
    IF (WITH_CTC_STORAGE_ENGINE AND NOT WITHOUT_CTC_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${CTC_SOURCES}
            STORAGE_ENGINE 
            MANDATORY
            LINK_LIBRARIES libzecommon.so libzeclient.so libzeprotocol.so libprotobuf-c.a pcre2-8 ctc_proxy daac
        )
    ELSEIF (NOT WITHOUT_CTC_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${CTC_SOURCES} 
            STORAGE_ENGINE 
            MODULE_ONLY
            LINK_LIBRARIES libzecommon.so libzeclient.so libzeprotocol.so libprotobuf-c.a pcre2-8 ctc_proxy daac
        )
    ENDIF ()
ELSE ()
    IF (WITH_CTC_STORAGE_ENGINE AND NOT WITHOUT_CTC_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${CTC_SOURCES} 
            STORAGE_ENGINE 
            DEFAULT
            LINK_LIBRARIES message_queue libprotobuf-c.a libsecurec.so ctc_proxy
        )
    ELSEIF (NOT WITHOUT_CTC_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${CTC_SOURCES} 
            STORAGE_ENGINE 
            MODULE_ONLY
            LINK_LIBRARIES message_queue libprotobuf-c.a libsecurec.so ctc_proxy
        )
    ENDIF ()
ENDIF ()

