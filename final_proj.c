#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/resmgr.h>
#include <sys/netmgr.h>

#define MY_PULSE_CODE      _PULSE_CODE_MINAVAIL
#define PAUSE_PULSE_CODE   _PULSE_CODE_MINAVAIL+1
#define QUIT_PULSE_CODE    _PULSE_CODE_MINAVAIL+2

double returnTimerSecs(char bpm[8], char topsig[8], char botsig[8]);

char data[255];
int server_coid;
pthread_t metronome;
char topsig[8];
char botsig[8];
char bpm[8];
char output[16];

const char* metronomeTable[8][4] = {
		{"2", "4", "4", "|1&2&"},
		{"3", "4", "6", "|1&2&3&"},
		{"4", "4", "8", "|1&2&3&4&"},
		{"5", "4", "10", "|1&2&3&4-5-"},
		{"3", "8", "6", "|1-2-3-"},
		{"6", "8", "6", "|1&a2&a"},
		{"9", "8", "9", "|1&a2&a3&a"},
		{"12", "8", "12", "|1&a2&a3&a4&a"}};

typedef union {
	struct _pulse pulse;
	char msg[255];
} my_message_t;

int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
	if (data == NULL) return 0;

	int nb;

	nb = strlen(data);

	//test to see if we have already sent the whole message.
	if (ocb->offset == nb)
		return 0;

	//We will return which ever is smaller the size of our data or the size of the buffer
	nb = min(nb, msg->i.nbytes);

	//Set the number of bytes we will return
	_IO_SET_READ_NBYTES(ctp, nb);

	//Copy data into reply buffer.
	SETIOV(ctp->iov, data, nb);

	//update offset into our data used to determine start position for next read.
	ocb->offset += nb;

	//If we are going to send any bytes update the access time for this resource.
	if (nb > 0)
		ocb->attr->flags |= IOFUNC_ATTR_ATIME;

	return(_RESMGR_NPARTS(1));
}

int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
	int nb = 0;

	if( msg->i.nbytes == ctp->info.msglen - (ctp->offset + sizeof(*msg) ))
	{
		/* have all the data */
		char *buf;
		char *pause_num;
		int i, pause_number;
		buf = (char *)(msg+1);

		if (strstr(buf, "pause") != NULL){
			for(i = 0; i < 2; i++){
				pause_num = strsep(&buf, " ");
			}
			pause_number = atoi(pause_num);
			//printf("<pause %d>", pause_number);
			if(pause_number >= 1 && pause_number <= 9){
				MsgSendPulse(server_coid, SchedGet(0,0,NULL), _PULSE_CODE_MINAVAIL+1, pause_number);
			} else {
				printf("\nYou must pause between 1-9 seconds.\n");
			}
		}
		else if (strstr(buf, "info") != NULL){
			snprintf(data,sizeof data,"metronome [%s beats/min, time signature %s/%s]",
					bpm, topsig, botsig);
			//strcpy(data, "metronome [<bpm> beats/min, time signature <ts-top>/<ts-bottom>");
		}
		else if (strstr(buf, "quit") != NULL){
			MsgSendPulse(server_coid, SchedGet(0,0,NULL), _PULSE_CODE_MINAVAIL+2, 1);
			name_close(server_coid);
			pthread_cancel(metronome);
			exit(EXIT_SUCCESS);
		}
		else {
			fprintf(stderr, "\nError - '%s' is not a valid command\n", buf);
			//stderr("Error – ‘%s’ is not a valid command");
			strcpy(data, buf);
		}

		nb = msg->i.nbytes;
	}
	_IO_SET_WRITE_NBYTES (ctp, nb);

	if (msg->i.nbytes > 0)
		ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;

	return (_RESMGR_NPARTS (0));
}

int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra)
{
	if ((server_coid = name_open("metronome", 0)) == -1) {
		perror("name_open failed.");
		return EXIT_FAILURE;
	}
	return (iofunc_open_default (ctp, msg, handle, extra));
}

void *childThread (void *arg) {
	struct sigevent event;
	struct itimerspec itime;
	double timerInterval;
	timer_t timer_id;
	int i = 0;
	int rcvid;
	my_message_t msg;
	name_attach_t *nameAttach;
	char tempBuffer[256];

	if ((nameAttach = name_attach(NULL, "metronome", 0)) == NULL) {
		perror("myController name_attach failed");
		exit(EXIT_FAILURE);
	}

	printf("Thread running\n");

	timerInterval = returnTimerSecs(bpm, topsig, botsig);

	event.sigev_notify = SIGEV_PULSE;
	event.sigev_coid = ConnectAttach(ND_LOCAL_NODE, 0,
			nameAttach->chid,
			_NTO_SIDE_CHANNEL, 0);
	event.sigev_priority = SchedGet(0,0,NULL);
	event.sigev_code = MY_PULSE_CODE;

	if (timer_create(CLOCK_REALTIME, &event, &timer_id) == -1) {
		perror("timer_create failed");
		exit(EXIT_FAILURE);
	}

	itime.it_value.tv_sec = 1;
	/* 500 million nsecs = .5 secs */
	itime.it_value.tv_nsec = 500000000;
	//itime.it_interval.tv_sec = 1;
	/* 500 million nsecs = .5 secs */
	itime.it_interval.tv_nsec = 1000000000*timerInterval;
	timer_settime(timer_id, 0, &itime, NULL);

	memset(tempBuffer, 0, 256);

	while (1) {
		rcvid = MsgReceive(nameAttach->chid, &msg, sizeof(msg), NULL);
		if (rcvid == 0) {
			if (msg.pulse.code == MY_PULSE_CODE) {
				if (i == strlen(output)) {
					printf("\n");
					i = 0;
				}

				printf("%c", output[i]);
				fflush(stdout);
				i++;
			}
			else if (msg.pulse.code == PAUSE_PULSE_CODE) {
				printf("<pause %d>", msg.pulse.value.sival_int);
				fflush(stdout);
				//event.sigev_notify = SIGEV_UNBLOCK;
				itime.it_value.tv_sec = msg.pulse.value.sival_int;
				itime.it_value.tv_nsec = 0;
				itime.it_interval.tv_nsec = 1000000000*timerInterval;

				timer_settime(timer_id, 0, &itime, NULL);
			}

			else if (msg.pulse.code == QUIT_PULSE_CODE) {
				name_detach(nameAttach, 0);
				timer_delete(timer_id);
				exit(EXIT_SUCCESS);
			}
		}

		else {
			snprintf(msg.msg, sizeof msg.msg, "MsgReceivePulse failed");
			perror("Pulse not received");

			if (MsgReply(rcvid, 0, &msg, sizeof(msg)) == -1) {
				perror("controller MsgReply failed");
				exit(EXIT_FAILURE);
			}

			name_detach(nameAttach, 0);
			//fclose(fd);
			exit(EXIT_FAILURE);
		}
	}
	name_detach(nameAttach, 0);
}

double returnTimerSecs(char bpmP[8], char sigtopP[8], char sigbotP[8]) {
	int i;
	char intervals[8];
	double bpm = atoi(bpmP);
	double topsig = atoi(sigtopP);
	double bps = 60 / bpm;
	double spm = bps*topsig;
	for (i = 0; i < 8; i++) {
		if (strcmp((metronomeTable[i][0]),sigtopP) == 0) {
			if (strcmp((metronomeTable[i][1]),sigbotP) == 0) {
				strcpy(intervals, metronomeTable[i][2]);
				strcpy(output, metronomeTable[i][3]);
			}
		}
	}

	double secondsPerInterval = spm / atoi(intervals);
	return secondsPerInterval;
}

int main(int argc, char *argv[]) {

	pthread_attr_t attr;
	resmgr_attr_t resmgr_attr;
	dispatch_t *dispatch;
	dispatch_context_t *dispatch_context;
	resmgr_connect_funcs_t resmgr_connect_funcs;
	resmgr_io_funcs_t resmgr_io_funcs;
	iofunc_attr_t iofunc_attr;

	if (argc != 4) {
		perror("Metronome Usage: \nYou MUST put 3 arguments in this order: <beats-per-minute> <time-signature-top> <time-signature-bottom> \n");
		exit(EXIT_FAILURE);
	}

	sscanf(argv[1],"%s", &bpm);
	sscanf(argv[2],"%s", &topsig);
	sscanf(argv[3],"%s", &botsig);


	if ((dispatch = dispatch_create()) == NULL) {
		perror("dispatch_create failed!");
		exit (EXIT_FAILURE);
	}

	memset(&resmgr_attr, 0, sizeof(resmgr_attr));
	resmgr_attr.nparts_max = 1;
	resmgr_attr.msg_max_size = 256;

	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &resmgr_connect_funcs, _RESMGR_IO_NFUNCS, &resmgr_io_funcs);
	resmgr_connect_funcs.open = io_open;
	resmgr_io_funcs.read = io_read;
	resmgr_io_funcs.write = io_write;

	iofunc_attr_init(&iofunc_attr, S_IFNAM | 0666, 0, 0);

	if (resmgr_attach(dispatch, &resmgr_attr, "/dev/local/metronome", _FTYPE_ANY, 0, &resmgr_connect_funcs,
			&resmgr_io_funcs, &iofunc_attr) == -1) {
		perror("resmgr_attach failed!");
		exit (EXIT_FAILURE);
	}

	dispatch_context = dispatch_context_alloc(dispatch);

	pthread_attr_init(&attr);
	metronome = pthread_create(NULL, &attr, &childThread, NULL);
	pthread_attr_destroy(&attr);

	while (1) {
		if ((dispatch_context = dispatch_block(dispatch_context)) == NULL) {
			perror("dispatch_block failed!");
			exit (EXIT_FAILURE);
		}
		dispatch_handler (dispatch_context);

	}
	//wont ever get to here
	exit(EXIT_SUCCESS);
}
