#!/bin/sh

. ./regress.conf

gfs_pio_test=`dirname $testbin`/gfs_pio_test/gfs_pio_test

trap 'gfrm -f $gftmp; rm -f $localtmp; exit $exit_trap' $trap_sigs

if
   # gfs_pio_write
   $gfs_pio_test $* -ctw -O $gftmp <$data/65byte &&
   $regress/bin/is_cksum_same $gftmp $data/65byte &&
   gfrm -f $gftmp &&

   # gfs_pio_write, no calculation
   $gfs_pio_test $* -ctw -S 1 -O $gftmp <$data/65byte &&
   [ X"`gfcksum -t $gftmp`" = X"" ] &&
   gfrm -f $gftmp &&

   # gfs_pio_truncate to 0
   $gfs_pio_test $* -ctw -O -T 0 $gftmp <$data/65byte &&
   $regress/bin/is_cksum_same $gftmp /dev/null &&
   gfrm -f $gftmp &&

   # gfs_pio_truncate, rewind and gfs_pio_write
   cat $data/65byte $data/65byte |
   $gfs_pio_test $* -ctw -W 65 -T 0 -S 0 -O $gftmp &&
   $regress/bin/is_cksum_same $gftmp $data/65byte &&
   gfrm -f $gftmp &&

   # gfs_pio_truncate to shorter size, no calculation
   $gfs_pio_test $* -ctw -O -T 64 $gftmp <$data/65byte &&
   [ X"`gfcksum -t $gftmp`" = X"" ] &&
   gfrm -f $gftmp &&

   # gfs_pio_truncate to just same size
   $gfs_pio_test $* -ctw -O -T 65 $gftmp <$data/65byte &&
   $regress/bin/is_cksum_same $gftmp $data/65byte &&
   gfrm -f $gftmp &&

   # gfs_pio_truncate to longer size, no calculation
   $gfs_pio_test $* -ctw -O -T 66 $gftmp <$data/65byte &&
   [ X"`gfcksum -t $gftmp`" = X"" ] &&
   gfrm -f $gftmp &&

   # gfs_pio_truncate to longer size
   ( cat $data/65byte; awk 'BEGIN {printf "%c", 0; exit}' ) >$localtmp &&
   $gfs_pio_test $* -ct -O -T 66 -S 65 -I $gftmp <$data/65byte >/dev/null &&
   $regress/bin/is_cksum_same $gftmp $localtmp &&
   rm -f $localtmp &&
   gfrm -f $gftmp &&

   # appened (but, libgfarm may take it as writing whole while), no calculation
   gfreg $* $data/65byte $gftmp &&
   $gfs_pio_test $* -a -O $gftmp <$data/65byte &&
   [ X"`gfcksum -t $gftmp`" = X"" ] &&
#   gfrm -f $gftmp &&

   true

then
    exit_code=$exit_pass
fi

gfrm -f $gftmp
rm -f $localtmp
exit $exit_code
