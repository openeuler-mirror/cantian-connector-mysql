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
SET(TSE_PLUGIN_DYNAMIC "ha_ctc")
SET(TSE_SOURCES tse_log.h ha_tse.cc ha_tse.h ha_tsepart.h ha_tsepart.cc ha_tse_ddl.cc ha_tse_ddl.h tse_ddl_util.cc tse_ddl_util.h datatype_cnvrtr.cc datatype_cnvrtr.h tse_error.cc tse_error.h
                decimal_convert.cc decimal_convert.h tse_cbo.cc tse_cbo.h datatype_cnvrt_4_index_search.cc datatype_cnvrt_4_index_search.h tse_ddl_rewriter_plugin.cc
                ctc_meta_data.cc ctc_meta_data.h tse_stats.h tse_stats.cc tse_srv.h tse_util.h tse_util.cc protobuf/tc_db.pb-c.c protobuf/tc_db.pb-c.h)
option(WITH_DAAC OFF)
IF (WITH_DAAC)
    add_definitions(-DWITH_DAAC)
    message(STATUS "ADD WITH DAAC")
    list(APPEND TSE_SOURCES mysql_daac_plugin.cc)
ELSE ()
    list(APPEND TSE_SOURCES tse_srv_mq_stub.cc tse_srv_mq_module.cc tse_srv_mq_module.h srv_mq_msg.h message_queue/dsw_shm.h 
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
SET(DAAC_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../daac_lib)
SET(MYSQL_CLIENT_STATIC_LIB_PATH ${CMAKE_BINARY_DIR}/archive_output_directory/)
LINK_DIRECTORIES(${DAAC_LIB_PATH})
LINK_DIRECTORIES(${MYSQL_CLIENT_STATIC_LIB_PATH})
INCLUDE_DIRECTORIES(SYSTEM ${BOOST_PATCHES_DIR} ${BOOST_INCLUDE_DIR})

SET(TSE_PROXY_SOURCES tse_proxy_util.cc tse_proxy_util.h tse_mysql_proxy.cc)
SET(TSE_PROXY_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-Bsymbolic")
add_library(ctc_proxy SHARED ${TSE_PROXY_SOURCES})
target_link_libraries(ctc_proxy libmysqlclient.a)
# ctc_proxy link libmysqlclient.a function symbol rather than mysqld`s
set_target_properties(ctc_proxy PROPERTIES COMPILE_FLAGS ${TSE_PROXY_CXX_FLAGS} LINK_FLAGS " -Wl,-Bsymbolic"
    LIBRARY_OUTPUT_DIRECTORY ${DAAC_LIB_PATH})

IF (WITH_DAAC)
    IF (WITH_TSE_STORAGE_ENGINE AND NOT WITHOUT_TSE_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${TSE_SOURCES}
            STORAGE_ENGINE 
            MANDATORY
            LINK_LIBRARIES libzecommon.so libzeclient.so libzeprotocol.so libprotobuf-c.a pcre2-8 ctc_proxy daac
        )
    ELSEIF (NOT WITHOUT_TSE_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${TSE_SOURCES} 
            STORAGE_ENGINE 
            MODULE_ONLY
            LINK_LIBRARIES libzecommon.so libzeclient.so libzeprotocol.so libprotobuf-c.a pcre2-8 ctc_proxy daac
        )
    ENDIF ()
ELSE ()
    IF (WITH_TSE_STORAGE_ENGINE AND NOT WITHOUT_TSE_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${TSE_SOURCES} 
            STORAGE_ENGINE 
            DEFAULT
            LINK_LIBRARIES message_queue libprotobuf-c.a libsecurec.so ctc_proxy
        )
    ELSEIF (NOT WITHOUT_TSE_STORAGE_ENGINE)
        MYSQL_ADD_PLUGIN(ctc 
            ${TSE_SOURCES} 
            STORAGE_ENGINE 
            MODULE_ONLY
            LINK_LIBRARIES message_queue libprotobuf-c.a libsecurec.so ctc_proxy
        )
    ENDIF ()
ENDIF ()

