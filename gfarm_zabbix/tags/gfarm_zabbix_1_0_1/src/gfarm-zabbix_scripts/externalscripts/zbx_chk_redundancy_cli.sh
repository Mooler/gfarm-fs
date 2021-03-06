#!/bin/sh

# defines
CONF_FILE=/etc/zabbix/externalscripts/zbx_chk_gfarm.conf

# Check Config File
if [ -f $CONF_FILE ];
    then
    . $CONF_FILE
else
    echo  -1;
    exit 0;
fi

# Check metadata server list
if [ ! -f $MDS_LIST_PATH ]; then
    echo  -1;
    exit 0;
fi

# exec check command
RESULT=`grep "^+ \+slave \+sync \+c \+" $MDS_LIST_PATH | wc -l`

echo $RESULT
exit 0
