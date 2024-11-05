# mtr使用文档

MySQL 测试框架是一个以 MySQL 框架和内部引擎为测试对象的工具，主要执行脚本在安装路径下的`mysql-test`目录，基本覆盖了所有 MySQL 的特性和异常情况。

本文档主要说明如何使用mtr脚本进行测试，覆盖场景为按照cantian文档部署的容器环境下的单进程，双进程，归一以及非归一场景。物理机场景则需要根据实际环境修改对应路径。

# 文件目录

 - `enableCases.list` 本地mtr测试使用的用例列表
 - `enableCases-euler.list` euler使用的用例列表
 - `mysql-scripts-meta.patch` `mysql-source-code-meta.patch` `mysql-test-meta.patch` 归一版本部署需要打patch使用的patch文件
 - `mysql-test-run.pl` 双进程非归一版本使用此mtr脚本
 - `mysql-test-run-meta.pl` 双进程归一版本使用此mtr脚本
 - `mysql-test-run-meta-single.pl` 单进程归一版本使用此mtr脚本
 - `mysql-test-run-single.pl` 单进程非归一版本使用此mtr脚本

# 双进程

双进程场景执行mtr前不需要初始化和部署MySQL，仅需要完成cantian和MySQL的编译以及cantian的部署。

## 非归一

```shell
cd /usr/local/mysql/mysql-test 
chmod 777 ./mysql-test-run.pl
```
执行mtr脚本
```shell
./mysql-test-run.pl --mysqld=--plugin_load="ctc_ddl_rewriter=ha_ctc.so;ctc=ha_ctc.so;" --mysqld=--default-storage-engine=CTC --mysqld=--check_proxy_users=ON --mysqld=--mysql_native_password_proxy_users=ON --do-test-list=enableCases.list --noreorder
```

## 归一

```shell
cd /usr/local/mysql/mysql-test 
chmod 777 ./mysql-test-run-meta.pl
```
执行mtr脚本
```shell
./mysql-test-run-meta.pl --mysqld=--default-storage-engine=CTC --mysqld=--check_proxy_users=ON --do-test-list=enableCases.list --noreorder --nowarnings
```

# 单进程

单进程mtr执行操作与双进程不同，双进程场景下可以直接由mtr脚本进行MySQL的初始化和拉起，单进程则因为cantian的初始化嵌入了MySQL中，需要提前进行MySQL的初始化和拉起，再关闭MySQL进程，使用适配后的mtr脚本进行测试。

## 准备工作

由于mtr初始化的特殊性，需要提取mtr初始化的sql文件，并且使用和脚本相同的初始化语句进行MySQL的初始化。初始化使用的bootstrap.sql文件位于/pkg/admin/scripts/目录下，初始化语句则需要修改cantian代码中installdb.sh中的初始化语句。
```shell
${MYSQL_BIN_DIR}/bin/mysqld  --defaults-file=${MYSQL_CONFIG_FILE}  --initialize-insecure  --datadir=${MYSQL_DATA_DIR}  --early-plugin-load="ha_ctc.so"  --core-file
```
修改为
```shell
${MYSQL_BIN_DIR}/bin/mysqld --no-defaults --initialize-insecure --tmpdir=${MYSQL_BIN_DIR}/mysql-test/var/tmp/ --core-file --datadir=${MYSQL_DATA_DIR} --secure-file-priv=${MYSQL_BIN_DIR}/mysql-test/var --early-plugin-load=ha_ctc.so --character-sets-dir=${MYSQL_BIN_DIR}/share/charsets --loose-auto_generate_certs=OFF --loose-sha256_password_auto_generate_rsa_keys=OFF --loose-caching_sha2_password_auto_generate_rsa_keys=OFF --init-file=/home/regress/CantianKernel/pkg/admin/scripts/bootstarp.sql
```
其中初始化文件的路径需要设置为实际放置bootstrap.sql文件的位置(物理机默认路径为/opt/cantian/mysql/scripts/bootstrap.sql)。

MySQL安装部署完成后需要手动关闭MySQL，另外cms会在MySQL下线后拉起cantiand，所以保险起见一并关闭。
```shell
kill -9 $(pidof mysqld)
kill -9 $(pidof cantiand)
```
脚本赋权
```shell
cd /usr/local/mysql/mysql-test 
chmod 777 ./mysql-test-run-single.pl
chmod 777 ./mysql-test-run-meta-single.pl
```
由于单进程下MySQL以及cantian均使用cantiandba用户部署，所以执行mtr脚本也需要使用此用户，否则会出现权限不匹配问题。另外需要输出单进程MySQL部署必须的环境变量
```shell
su - cantiandba
cd /usr/local/mysql/mysql-test
export RUN_MODE=cantiand_with_mysql
export LD_LIBRARY_PATH=/usr/local/mysql/lib:/home/regress/cantian-connector-mysql/mysql-source/cantian_lib:/usr/local/mysql/lib:/home/regress/cantian-connector-mysql/mysql-source/cantian_lib:/usr/local/mysql/lib/private:/home/cantiandba/install/lib:/home/cantiandba/install/add-ons::/home/regress/cantian-connector-mysql/bld_debug/library_output_directory
export CANTIAND_MODE=open
export CANTIAND_HOME_DIR=/home/cantiandba/data
```

### 注
-   测试机环境需要根据实际安装路径修改初始化语句中的安装路径。
-   测试机环境需要额外修改下文单进程mtr脚本中 `my  $install_db`变量的值为实际安装的MySQL数据存放的路径。
-   执行mtr脚本遇到无法拉起MySQL服务器的问题可以检查是否有mysqld以及cantiand进程残留并关闭。
-   测试机中环境变量的设置也需要根据实际路径做匹配。
## 非归一

执行mtr脚本
```shell
./mysql-test-run-single.pl --mysqld=--check_proxy_users=ON --noreorder --nowarnings --force --retry=0 --do-test-list=enableCases.list
```

## 归一

执行mtr脚本
```shell
./mysql-test-run-meta-single.pl --mysqld=--check_proxy_users=ON --noreorder --nowarnings --force --retry=0 --do-test-list=enableCases.list
```

# mtr常见参数

-   `--force`
    -   默认情况下，只要遇到一个 case 出错，测试程序就会退出。
    -   加入该参数后，mtr 会忽略错误并继续执行下一个 case 直到所有 case 执行结束再退出。
    -   但如果脚本存在太多错误还是会退出，可设置`--max-test-fail=0`  忽略计数。
-   `--max-test-fail`
    -   测试过程中失败 case 数达到一定值会退出，默认值是 10，设置为 0 则会忽略计数。
-   `--record`
    -   **是否记录 results 结果，首次执行建议带上，让其自动生成  `.results`  文件，再基于该文件修改成我们预期的结果**。
    -   若一个执行输出结果和  `testname.result`  文件不同，会生成一个  `testname.reject`  文件，该文件在下次执行成功之后被删除；
    -   检查`.reject`文件的内容，如果里面是期望的输出，则将内容拷贝到  `.result`  文件中，作为以后判断运行结果是否通过的依据；
-   `--parallel`
    -   指定运行测试 case 的并行线程数。
-   `—-nowarnings`
    -   忽略 warnings 错误。
    -   设置该参数后，当出现 warnings 错误，不再累加  `--max-test-fail`  。
-   `--big-test`
    -   执行标记为  `big`  的 test cases，也就是同时覆盖  **非 big + big**。这是因为标记为 big 的 case 较大、耗时较长，默认不会执行。