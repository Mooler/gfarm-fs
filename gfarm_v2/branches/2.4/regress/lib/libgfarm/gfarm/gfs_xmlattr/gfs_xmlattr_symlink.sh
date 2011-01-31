#!/bin/sh

. ./regress.conf

tmp_file=$localtmp.txt
attr_file=$localtmp.xml
gf_file=$gftmp
gf_link=${gftmp}-l
setxattr_val="<a>setxattr</a>"
lsetxattr_val="<a>lsetxattr</a>"
xml_mode=-x

# is XML attr supported?
if gfxattr -x -g / test 2>&1 |
	egrep '^gfxattr: (unknown|invalid) option -- x$' >/dev/null
then
	exit $exit_unsupported
fi

clean_test() {
	rm -f $attr_file $tmp_file > /dev/null 2>&1
	gfrm -f $gf_link $gf_file > /dev/null 2>&1
}

clean_fail() {
	echo $*
	clean_test
	exit $exit_fail
}

trap 'clean_test; exit $exit_trap' $trap_sigs

# make test data
echo > $tmp_file
if gfreg $tmp_file $gf_file; then :
else
	clean_fail "gfreg failed"
fi

if gfln -s $gf_file $gf_link; then :
else
	clean_fail "gfln failed"
fi

# set attr
echo $setxattr_val > $attr_file

if gfxattr $xml_mode -s -f $attr_file $gf_link testattr; then :
else
	clean_fail "gfxattr set#1 failed"
fi

if gfxattr $xml_mode -s -f $attr_file $gf_link testattr2; then :
else
	clean_fail "gfxattr set#2 failed"
fi

echo $lsetxattr_val > $attr_file

if gfxattr $xml_mode -s -h -f $attr_file $gf_link testattr; then :
else
	clean_fail "gfxattr set#3 failed"
fi

# list attr
list1=`gfxattr $xml_mode -l $gf_file`
if [ "$?" != "0" ]; then
	clean_fail "gfxattr list#1 failed"
fi
list2=`gfxattr $xml_mode -l $gf_link`
if [ "$?" != "0" ]; then
	clean_fail "gfxattr list#2 failed"
fi
if [ "$list1" != "$list2" ]; then
	clean_fail "list1 and list2 is different : $list1, $list2"
fi
list3=`gfxattr $xml_mode -l -h $gf_link`
if [ "$?" != "0" ]; then
	clean_fail "gfxattr list#3 failed"
fi
if [ "$list1" = "$list3" ]; then
	clean_fail "list1 and list3 is equal : $list1, $list3"
fi
if [ "$list3" != "testattr" ]; then
	clean_fail "list3 is incorrect : $list3"
fi

# get attr
val1=`gfxattr $xml_mode -g $gf_file testattr`
if [ "$?" != "0" ]; then :
	clean_fail "gfxattr get#1 failed"
fi
if [ "$val1" != "$setxattr_val" ]; then
	clean_fail "xattr in $gf_file is incorrect. ($val1)"
fi

val2=`gfxattr $xml_mode -g $gf_link testattr`
if [ "$?" != "0" ]; then :
	clean_fail "gfxattr get#2 failed"
fi
if [ "$val2" != "$setxattr_val" ]; then
	clean_fail "xattr in $gf_file is incorrect. ($val2)"
fi

val3=`gfxattr $xml_mode -g -h $gf_link testattr`
if [ "$?" != "0" ]; then :
	clean_fail "gfxattr get#3 failed"
fi
if [ "$val3" != "$lsetxattr_val" ]; then
	clean_fail "xattr in $gf_link is incorrect. ($val3)"
fi

# remove attr

if gfxattr $xml_mode -r $gf_link testattr; then :
else
	clean_fail "gfxattr remove#1 failed"
fi
list1=`gfxattr $xml_mode -l $gf_file`
if [ "$?" != "0" ]; then
	clean_fail "gfxattr list#1 failed"
fi
if [ "$list1" != "testattr2" ]; then
	clean_fail "list1 is incorrect : $list1"
fi
if gfxattr $xml_mode -r -h $gf_link testattr; then :
else
	clean_fail "gfxattr remove#2 failed"
fi
list2=`gfxattr $xml_mode -l -h $gf_link`
if [ "$?" != "0" ]; then
	clean_fail "gfxattr list#4 failed"
fi
if [ "$list2" != "" ]; then
	clean_fail "list2 is incorrect : $list2"
fi

echo OK
clean_test
exit $exit_pass
