/* Socket-backed PCM test of the real collector lifecycle. No hardware IO. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static int pcm_socket = -1;
static int fake_hook_on = 1, hook_reads;
static int collector_exists(void);
static int fake_open(const char *path, int flags, ...)
{
	(void)flags;
	if (!strcmp(path, "/test-pcm")) return dup(pcm_socket);
	if (strstr(path, "/hook")) {
		int fds[2]; assert(!pipe(fds));
		/* Simulate the SLIC's transient false on-hook after PCM close. */
		char value = fake_hook_on && collector_exists() ? '1' : '0';
		assert(write(fds[1], &value, 1) == 1); close(fds[1]); hook_reads++;
		return fds[0];
	}
	return -1;
}
#define open fake_open
#define main callmgr_program_main
#include "../overlay/package/xr500v-voip/src/xr500v-callmgr.c"
#undef main
#undef open
static int collector_exists(void) { return keypad_pid > 0; }

int main(void)
{
	int pcm[2], control[2];
	char path[]="/tmp/voip-keypad-test-XXXXXX", uri[380], response[1600];
	int file=mkstemp(path);
	assert(file>=0);
	const char fixture[]="keypad\nlocal\n192.0.2.1\n";
	assert(write(file,fixture,sizeof(fixture)-1)==sizeof(fixture)-1);close(file);
	strcpy(dialing_path,path);strcpy(keypad_device,"/test-pcm");isolated_ec=2;
	assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pcm));pcm_socket=pcm[0];
	assert(!socketpair(AF_UNIX,SOCK_STREAM,0,control));
	alarm(10);
	assert(keypad_start(control[0],-1)==1);
	for(int n=0;n<5;n++) {
		int16_t samples[160], playback[160];
		for(int i=0;i<160;i++) samples[i]=2000*(sin(2*M_PI*697*(i+n*160)/8000)+sin(2*M_PI*1336*(i+n*160)/8000));
		assert(!frame_io(pcm[1],samples,sizeof(samples),1));
		assert(!frame_io(pcm[1],playback,sizeof(playback),0));
	}
	struct pollfd p={keypad_fd,POLLIN,0};assert(poll(&p,1,1000)==1);
	char digit;assert(read(keypad_fd,&digit,1)==1 && digit=='2');
	assert(read(keypad_fd,&digit,1)<0 && errno==EAGAIN); /* held key only once */
	digit_count=1;strcpy(dial_digits,"2");state=COLLECTING;
	assert(!keypad_finish(control[0],2,NULL));
	assert(state==DIALING && hook_reads==1 && keypad_pid==-1 && keypad_fd==-1);
	int sent=read(control[1],response,sizeof(response)-1);assert(sent>0);response[sent]=0;
	assert(strstr(response,"sip:phone2@192.0.2.1:5062"));
	assert(read_hook()==0); /* proves a post-stop hook test would cancel */
	assert(waitpid(-1,NULL,WNOHANG)==-1 && errno==ECHILD);
	assert(keypad_start(control[0],-1)==1);
	digit_count=1;strcpy(dial_digits,"2");state=COLLECTING;fake_hook_on=0;
	assert(keypad_finish(control[0],2,NULL)==-1 && state==IDLE && keypad_pid==-1);
	struct pollfd no_dial={control[1],POLLIN,0};assert(!poll(&no_dial,1,10));
	fake_hook_on=1;
	digit_count=1;strcpy(dial_digits,"2");assert(!keypad_uri(uri,sizeof(uri)));
	assert(!strcmp(uri,"sip:phone2@192.0.2.1:5062"));
	strcpy(dial_digits,"1");assert(keypad_uri(uri,sizeof(uri))==-1); /* no self-call */
	strcpy(dial_kind,"provider");strcpy(dial_domain,"sip.invalid:5060");strcpy(dial_digits,"123");digit_count=3;
	assert(!keypad_uri(uri,sizeof(uri)) && !strcmp(uri,"sip:123@sip.invalid:5060"));
	assert(!ctrl_cmd(control[0],"dial","sip:foo\"bar@invalid"));
	int n=read(control[1],response,sizeof(response)-1);assert(n>0);response[n]=0;
	assert(strstr(response,"foo\\\"bar"));
	assert(ctrl_cmd(control[0],"dial","sip:bad\n@invalid")<0);
	memset(response,'a',400);response[400]=0;assert(ctrl_cmd(control[0],"dial",response)<0);
	close(pcm[0]);close(pcm[1]);close(control[0]);close(control[1]);unlink(path);
	puts("PASS: PCM worker, held keys, pre-release hook handoff, genuine on-hook cancellation, stop/reap, URI/self-call guard and JSON bounds");
}
