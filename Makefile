spawnshell: spawnshell.c
	gcc -g -Wall -Werror -o spawnshell spawnshell.c

clean:
	rm -f spawnshell

test: spawnshell
	rm -f testfileout
	./spawnshell < input1.sh | diff - output1.sh