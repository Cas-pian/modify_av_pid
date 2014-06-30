all:
	gcc -DHAVE_CONFIG_H -I. -I/usr/local/include -L/usr/local/lib -ldvbpsi -D_FILE_OFFSET_BITS=64 -DDVBPSI_DIST   -g -O2 -Wall --std=gnu99 -D_GNU_SOURCE -Wpointer-arith -Wcast-align -Wcast-qual -Wstrict-prototypes -Wshadow -Waggregate-return -Wmissing-prototypes -Wnested-externs -Wsign-compare -DDVBPSI_DIST -MT modify_av_pid-modify_av_pid.o -MD -MP -MF .deps/modify_av_pid-modify_av_pid.Tpo -c -o modify_av_pid-modify_av_pid.o `test -f 'modify_av_pid.c' || echo './'`modify_av_pid.c
	mv -f .deps/modify_av_pid-modify_av_pid.Tpo .deps/modify_av_pid-modify_av_pid.Po
	gcc -DHAVE_CONFIG_H -I. -I/usr/local/include  -D_FILE_OFFSET_BITS=64 -DDVBPSI_DIST   -g -O2 -Wall --std=gnu99 -D_GNU_SOURCE -Wpointer-arith -Wcast-align -Wcast-qual -Wstrict-prototypes -Wshadow -Waggregate-return -Wmissing-prototypes -Wnested-externs -Wsign-compare -DDVBPSI_DIST -MT modify_av_pid-connect.o -MD -MP -MF .deps/modify_av_pid-connect.Tpo -c -o modify_av_pid-connect.o `test -f 'connect.c' || echo './'`connect.c
	mv -f .deps/modify_av_pid-connect.Tpo .deps/modify_av_pid-connect.Po
	/bin/sh libtool  --tag=CC   --mode=link gcc  -g -O2 -Wall --std=gnu99 -D_GNU_SOURCE -Wpointer-arith -Wcast-align -Wcast-qual -Wstrict-prototypes -Wshadow -Waggregate-return -Wmissing-prototypes -Wnested-externs -Wsign-compare -DDVBPSI_DIST -ldvbpsi -lm  -o modify_av_pid modify_av_pid-modify_av_pid.o modify_av_pid-connect.o  

clean:
	rm -f *.o
	rm -f modify_av_pid

