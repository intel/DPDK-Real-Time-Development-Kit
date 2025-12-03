#!/bin/sh

#clang -S -Wno-int-to-void-pointer-cast --target=bpf -D__BPF_TRACING__ -Wall -O2 \
#	-fno-stack-protector -emit-llvm -g -I /usr/include/x86_64-linux-gnu \
#	-I examples/tsn/src -c -o builddir/examples/tsn/xdp_kern_avtp_vid400.ll \
#	examples/tsn/bpf/xdp_kern_avtp_vid400.c
#llc -march=bpf -filetype=obj -o builddir/examples/tsn/xdp_kern_avtp_vid400.o \
#	builddir/examples/tsn/xdp_kern_avtp_vid400.ll

build_dir=$1
src_dir=$2
infile=$3
asm_include=$4

#echo "ASM Include: " ${asm_include} >> /tmp/jnk
#echo "Build_dir:" ${build_dir} >> /tmp/jnk
#echo "Source_dir:" ${src_dir} >> /tmp/jnk

cflags="-S -Wno-int-to-void-pointer-cast -Wno-unused-command-line-argument --target=bpf -D__BPF_TRACING__ -Wall -O2 -fno-stack-protector -emit-llvm -g -I ${asm_include} -I ${src_dir}/src"

#echo clang ${cflags} -c -o ${build_dir}/${infile}.ll ${src_dir}/bpf/${infile}.c >> /tmp/jnk
clang ${cflags} -o ${build_dir}/${infile}.ll ${src_dir}/bpf/${infile}.c

#echo llc -march=bpf -filetype=obj -o ${build_dir}/${infile}.o ${build_dir}${infile}.ll >> /tmp/jnk
llc -march=bpf -filetype=obj -o ${build_dir}/${infile}.o ${build_dir}/${infile}.ll
