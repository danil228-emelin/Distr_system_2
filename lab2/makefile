PATH_TO_LIB :=../common
IPC_LIB :=IPC


build:
	clang -std=c99 bank_robbery.c -Llib64 -L../common -L. -lIPC -lruntime \
      -Wl,-rpath,./lib64:../common -o main

run: build
	./main –p 2 10 20