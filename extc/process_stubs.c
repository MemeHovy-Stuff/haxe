/*
 * Copyright (C)2005-2015 Haxe Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

 // ported from NekoVM

#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/mlvalues.h>
#include <caml/fail.h>

#ifdef _WIN32
#	include <windows.h>
#else
#	include <sys/types.h>
#	include <unistd.h>
#	include <errno.h>
#	include <string.h>
#	ifndef __APPLE__
#		include <wait.h>
#	endif
#endif

#ifdef _WIN32
#	define POSIX_LABEL(name)
#	define HANDLE_EINTR(label)
#	define HANDLE_FINTR(f,label)
#else
#	include <errno.h>
#	define POSIX_LABEL(name)	name:
#	define HANDLE_EINTR(label)	if( errno == EINTR ) goto label
#	define HANDLE_FINTR(f,label) if( ferror(f) && errno == EINTR ) goto label
#endif

// --- neko-to-caml api --
#define val_check(v,t)
#define val_check_kind(v,k)
#define val_data(v) v
#define val_array_size(v) Wosize_val(v)
#define val_array_ptr(v) (&Field(v,0))
#define val_string(v) String_val(v)
#define val_strlen(v) caml_string_length(v)
#define alloc_abstract(_,data) ((value)data)
#define alloc_int(i) Val_int(i)
#define val_gc(v,callb)
#define val_null Val_int(0)
#define val_int(v) Int_val(v)
#define DEFINE_KIND(_)
#define neko_error() failwith(__FUNCTION__)

static value alloc_private( int size ) {
	return alloc((size + sizeof(value) - 1) / sizeof(value), Abstract_tag);
}

// --- buffer api
#define EXTERN

typedef struct _stringitem {
	char *str;
	int size;
	int len;
	struct _stringitem *next;
} * stringitem;

struct _buffer {
	int totlen;
	int blen;
	stringitem data;
};

typedef struct _buffer *buffer;

static void buffer_append_new( buffer b, const char *s, int len ) {
	int size;
	stringitem it;
	while( b->totlen >= (b->blen << 2) )
		b->blen <<= 1;
	size = (len < b->blen)?b->blen:len;
	it = (stringitem)malloc(sizeof(struct _stringitem));
	it->str = (char*)malloc(size);
	memcpy(it->str,s,len);
	it->size = size;
	it->len = len;
	it->next = b->data;
	b->data = it;
}

EXTERN void buffer_append_sub( buffer b, const char *s, int len ) {
	stringitem it;
	if( s == NULL || len <= 0 )
		return;
	b->totlen += len;
	it = b->data;
	if( it ) {
		int free = it->size - it->len;
		if( free >= len ) {
			memcpy(it->str + it->len,s,len);
			it->len += len;
			return;
		} else {
			memcpy(it->str + it->len,s,free);
			it->len += free;
			s += free;
			len -= free;
		}
	}
	buffer_append_new(b,s,len);
}

EXTERN void buffer_append_str( buffer b, const char *s ) {
	if( s == NULL )
		return;
	buffer_append_sub(b,s,strlen(s));
}

EXTERN buffer alloc_buffer( const char *init ) {
	buffer b = (buffer)malloc(sizeof(struct _buffer));
	b->totlen = 0;
	b->blen = 16;
	b->data = NULL;
	if( init )
		buffer_append_str(b,init);
	return b;
}

EXTERN void buffer_append_char( buffer b, char c ) {
	stringitem it;
	b->totlen++;
	it = b->data;
	if( it && it->len != it->size ) {
		it->str[it->len++] = c;
		return;
	}
	buffer_append_new(b,&c,1);
}

EXTERN char *buffer_to_string( buffer b ) {
	char *v = (char*)malloc(b->totlen);
	stringitem it = b->data;
	char *s = (char*)val_string(v) + b->totlen;
	while( it != NULL ) {
		stringitem tmp;
		s -= it->len;
		memcpy(s,it->str,it->len);
		tmp = it->next;
		free(it->str);
		free(it);
		it = tmp;
	}
	free(b);
	return v;
}

EXTERN int buffer_length( buffer b ) {
	return b->totlen;
}

// ---------------

#include <stdio.h>
#include <stdlib.h>

typedef struct {
#ifdef _WIN32
	HANDLE oread;
	HANDLE eread;
	HANDLE iwrite;
	PROCESS_INFORMATION pinf;
#else
	int oread;
	int eread;
	int iwrite;
	int pid;
#endif
} vprocess;

DEFINE_KIND(k_process);

#define val_process(v)	((vprocess*)val_data(v))

/**
	<doc>
	<h1>Process</h1>
	<p>
	An API for starting and communication with sub processes.
	</p>
	</doc>
**/
#ifndef _WIN32
static int do_close( int fd ) {
	POSIX_LABEL(close_again);
	if( close(fd) != 0 ) {
		HANDLE_EINTR(close_again);
		return 1;
	}
	return 0;
}
#endif

static void free_process( value vp ) {
	vprocess *p = val_process(vp);
#	ifdef _WIN32
	CloseHandle(p->eread);
	CloseHandle(p->oread);
	CloseHandle(p->iwrite);
	CloseHandle(p->pinf.hProcess);
	CloseHandle(p->pinf.hThread);
#	else
	do_close(p->eread);
	do_close(p->oread);
	do_close(p->iwrite);
#	endif
}

/**
	process_run : cmd:string -> args:string array -> 'process
	<doc>
	Start a process using a command and the specified arguments.
	</doc>
**/
CAMLprim value process_run( value cmd, value vargs ) {
	int i;
	vprocess *p;
	val_check(cmd,string);
	val_check(vargs,array);
#	ifdef _WIN32
	{
		SECURITY_ATTRIBUTES sattr;
		STARTUPINFO sinf;
		HANDLE proc = GetCurrentProcess();
		HANDLE oread,eread,iwrite;
		// creates commandline
		buffer b = alloc_buffer(NULL);
		char *sargs;
		buffer_append_char(b,'"');
		buffer_append_str(b,val_string(cmd));
		buffer_append_char(b,'"');
		for(i=0;i<val_array_size(vargs);i++) {
			value v = val_array_ptr(vargs)[i];
			int j,len;
			val_check(v,string);
			len = val_strlen(v);
			buffer_append_str(b," \"");
			for(j=0;j<len;j++) {
				char c = val_string(v)[j];
				switch( c ) {
				case '"':
					buffer_append_str(b,"\\\"");
					break;
				case '\\':
					buffer_append_str(b,"\\\\");
					break;
				default:
					buffer_append_char(b,c);
					break;
				}
			}
			buffer_append_char(b,'"');
		}
		sargs = buffer_to_string(b);
		p = (vprocess*)alloc_private(sizeof(vprocess));
		// startup process
		sattr.nLength = sizeof(sattr);
		sattr.bInheritHandle = TRUE;
		sattr.lpSecurityDescriptor = NULL;
		memset(&sinf,0,sizeof(sinf));
		sinf.cb = sizeof(sinf);
		sinf.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		sinf.wShowWindow = SW_HIDE;
		CreatePipe(&oread,&sinf.hStdOutput,&sattr,0);
		CreatePipe(&eread,&sinf.hStdError,&sattr,0);
		CreatePipe(&sinf.hStdInput,&iwrite,&sattr,0);
		DuplicateHandle(proc,oread,proc,&p->oread,0,FALSE,DUPLICATE_SAME_ACCESS);
		DuplicateHandle(proc,eread,proc,&p->eread,0,FALSE,DUPLICATE_SAME_ACCESS);
		DuplicateHandle(proc,iwrite,proc,&p->iwrite,0,FALSE,DUPLICATE_SAME_ACCESS);
		CloseHandle(oread);
		CloseHandle(eread);
		CloseHandle(iwrite);
		if( !CreateProcess(NULL,val_string(sargs),NULL,NULL,TRUE,0,NULL,NULL,&sinf,&p->pinf) )
			neko_error();
		// close unused pipes
		CloseHandle(sinf.hStdOutput);
		CloseHandle(sinf.hStdError);
		CloseHandle(sinf.hStdInput);
	}
#	else
	char **argv = (char**)alloc_private(sizeof(char*)*(val_array_size(vargs)+2));
	argv[0] = val_string(cmd);
	for(i=0;i<val_array_size(vargs);i++) {
		value v = val_array_ptr(vargs)[i];
		val_check(v,string);
		argv[i+1] = val_string(v);
	}
	argv[i+1] = NULL;
	int input[2], output[2], error[2];
	if( pipe(input) || pipe(output) || pipe(error) )
		neko_error();
	p = (vprocess*)alloc_private(sizeof(vprocess));
	p->pid = fork();
	if( p->pid == -1 ) {
		do_close(input[0]);
		do_close(input[1]);
		do_close(output[0]);
		do_close(output[1]);
		do_close(error[0]);
		do_close(error[1]);
		neko_error();
	}
	// child
	if( p->pid == 0 ) {
		close(input[1]);
		close(output[0]);
		close(error[0]);
		dup2(input[0],0);
		dup2(output[1],1);
		dup2(error[1],2);
		execvp(val_string(cmd),argv);
		fprintf(stderr,"Command not found : %s\n",val_string(cmd));
		exit(1);
	}
	// parent
	do_close(input[0]);
	do_close(output[1]);
	do_close(error[1]);
	p->iwrite = input[1];
	p->oread = output[0];
	p->eread = error[0];
#	endif
	{
		value vp = alloc_abstract(k_process,p);
		val_gc(vp,free_process);
		return vp;
	}
}

#define CHECK_ARGS()	\
	vprocess *p; \
	val_check_kind(vp,k_process); \
	val_check(str,string); \
	val_check(pos,int); \
	val_check(len,int); \
	if( val_int(pos) < 0 || val_int(len) < 0 || val_int(pos) + val_int(len) > val_strlen(str) ) \
		neko_error(); \
	p = val_process(vp); \


/**
	process_stdout_read : 'process -> buf:string -> pos:int -> len:int -> int
	<doc>
	Read up to [len] bytes in [buf] starting at [pos] from the process stdout.
	Returns the number of bytes readed this way. Raise an exception if this
	process stdout is closed and no more data is available for reading.
	</doc>
**/
CAMLprim value process_stdout_read( value vp, value str, value pos, value len ) {
	CHECK_ARGS();
#	ifdef _WIN32
	{
		DWORD nbytes;
		if( !ReadFile(p->oread,val_string(str)+val_int(pos),val_int(len),&nbytes,NULL) )
			neko_error();
		return alloc_int(nbytes);
	}
#	else
	int nbytes;
	POSIX_LABEL(stdout_read_again);
	nbytes = read(p->oread,val_string(str)+val_int(pos),val_int(len));
	if( nbytes < 0 ) {
		HANDLE_EINTR(stdout_read_again);
		neko_error();
	}
	if( nbytes == 0 )
		neko_error();
	return alloc_int(nbytes);
#	endif
}

/**
	process_stderr_read : 'process -> buf:string -> pos:int -> len:int -> int
	<doc>
	Read up to [len] bytes in [buf] starting at [pos] from the process stderr.
	Returns the number of bytes readed this way. Raise an exception if this
	process stderr is closed and no more data is available for reading.
	</doc>
**/
CAMLprim value process_stderr_read( value vp, value str, value pos, value len ) {
	CHECK_ARGS();
#	ifdef _WIN32
	{
		DWORD nbytes;
		if( !ReadFile(p->eread,val_string(str)+val_int(pos),val_int(len),&nbytes,NULL) )
			neko_error();
		return alloc_int(nbytes);
	}
#	else
	int nbytes;
	POSIX_LABEL(stderr_read_again);
	nbytes = read(p->eread,val_string(str)+val_int(pos),val_int(len));
	if( nbytes < 0 ) {
		HANDLE_EINTR(stderr_read_again);
		neko_error();
	}
	if( nbytes == 0 )
		neko_error();
	return alloc_int(nbytes);
#	endif
}

/**
	process_stdin_write : 'process -> buf:string -> pos:int -> len:int -> int
	<doc>
	Write up to [len] bytes from [buf] starting at [pos] to the process stdin.
	Returns the number of bytes writen this way. Raise an exception if this
	process stdin is closed.
	</doc>
**/
CAMLprim value process_stdin_write( value vp, value str, value pos, value len ) {
	CHECK_ARGS();
#	ifdef _WIN32
	{
		DWORD nbytes;
		if( !WriteFile(p->iwrite,val_string(str)+val_int(pos),val_int(len),&nbytes,NULL) )
			neko_error();
		return alloc_int(nbytes);
	}
#	else
	int nbytes;
	POSIX_LABEL(stdin_write_again);
	nbytes = write(p->iwrite,val_string(str)+val_int(pos),val_int(len));
	if( nbytes == -1 ) {
		HANDLE_EINTR(stdin_write_again);
		neko_error();
	}
	return alloc_int(nbytes);
#	endif
}

/**
	process_stdin_close : 'process -> void
	<doc>
	Close the process standard input.
	</doc>
**/
CAMLprim value process_stdin_close( value vp ) {
	vprocess *p;
	val_check_kind(vp,k_process);
	p = val_process(vp);
#	ifdef _WIN32
	if( !CloseHandle(p->iwrite) )
		neko_error();
#	else
	if( do_close(p->iwrite) )
		neko_error();
	p->iwrite = -1;
#	endif
	return val_null;
}

/**
	process_exit : 'process -> int
	<doc>
	Wait until the process terminate, then returns its exit code.
	</doc>
**/
CAMLprim value process_exit( value vp ) {
	vprocess *p;
	val_check_kind(vp,k_process);
	p = val_process(vp);
#	ifdef _WIN32
	{
		DWORD rval;
		WaitForSingleObject(p->pinf.hProcess,INFINITE);
		if( !GetExitCodeProcess(p->pinf.hProcess,&rval) )
			neko_error();
		return alloc_int(rval);
	}
#	else
	int rval;
	while( waitpid(p->pid,&rval,0) != p->pid ) {
		if( errno == EINTR )
			continue;
		neko_error();
	}
	if( !WIFEXITED(rval) )
		neko_error();
	return alloc_int(WEXITSTATUS(rval));
#	endif
}

/**
	process_pid : 'process -> int
	<doc>
	Returns the process id.
	</doc>
**/
CAMLprim value process_pid( value vp ) {
	vprocess *p;
	val_check_kind(vp,k_process);
	p = val_process(vp);
#	ifdef _WIN32
	return alloc_int(p->pinf.dwProcessId);
#	else
	return alloc_int(p->pid);
#	endif
}

/**
	process_close : 'process -> void
	<doc>
	Close the process I/O.
	</doc>
**/
CAMLprim value process_close( value vp ) {
	val_check_kind(vp,k_process);
	free_process(vp);
	//val_kind(vp) = NULL;
	//val_gc(vp,NULL);
	return val_null;
}

/**
	process_kill : 'process -> void
	<doc>
	Terminates a running process.
	</doc>
**/
CAMLprim value process_kill( value vp ) {
	val_check_kind(vp,k_process);
#	ifdef _WIN32
	TerminateProcess(val_process(vp)->pinf.hProcess,-1);
#	else
	kill(val_process(vp)->pid,9);
#	endif
	return val_null;
}


/* ************************************************************************ */
