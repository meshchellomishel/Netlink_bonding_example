LIBS=/usr/lib/libnl-3.so /usr/lib/libnl-route-3.so
INCNL=/usr/include/libnl3/

conf: configtests.c Makefile
	gcc configtests.c -o conf -Wall -g

bond: Makefile bond.c
	gcc -g bond.c -lnl-3 -lnl-route-3 -I$(INCNL) -o bond

prio:
	gcc sysprio.c -o sysprio -Wall -g

clean:
	rm bond sysprio
