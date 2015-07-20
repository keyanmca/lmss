#ifndef __BILLING_LOG__H__
#define __BILLING_LOG__H__

#include    <unistd.h>
#include    <stdint.h>
#include    <stdlib.h>
#include    <pthread.h>
#include    <string.h>
#include    <sys/stat.h>
#include    <sys/types.h>
#include    <stdarg.h>
#include    <fcntl.h>
#include    <sys/socket.h>
#include    <netdb.h>
#include    <stdio.h>
#include    <sys/time.h>
#include    <sys/wait.h>
#include    <netinet/in.h>
#include    <arpa/inet.h>
#include    <errno.h>
#include    <assert.h>
#include    <regex.h>
#include    <signal.h>
#include    <stdbool.h>
#include    <sys/epoll.h>
#include	<pwd.h>
#include    <sys/file.h>
#include 	<signal.h>

#define MAX_PATH_NAME    		1024
#define MAX_FILE_NAME    		1024
#define MAX_CHANNELNAME  		1024
#define MAX_PIDFILE_BUF  		1024
#define MAX_VHOST        		20000
#define MAX_EVENTS       		1024
#define MAX_BILLING_BUF  		20000 * 100
#define EPOLL_TIMEOUT    		10
#define VERSION          		"1.0.0.0"

typedef struct billing_conf
{
	int  interval;  //second
	int  port;
	char path[MAX_PATH_NAME];
	char pid_path[MAX_PATH_NAME];
}billing_conf_t;

typedef struct billing_filename
{
	struct tm * time;
	char device_name[MAX_FILE_NAME];
}billing_filename_t;

typedef struct billing_data
{
	unsigned int Timestamp;
	unsigned int Interval;
	char ChannelName[MAX_CHANNELNAME];
	unsigned long bytes_from_client;
	unsigned long bytes_to_client;
}billing_data_t;

static billing_conf_t m_conf;
static billing_filename_t m_b_filename;
static billing_data_t m_b_data[MAX_VHOST];
static int m_listen_fd;
static int m_epoll_fd;
static struct epoll_event m_events[MAX_EVENTS];
static time_t firstlogtime;
unsigned int g_min;

static void  show_help (void);
static void  options_parse(int argc, char **argv);
static int   generate_pid_file(const char *pid_path);
static int   getdevice_name();
static void  create_socket();
static void  billingd_epoll_init(void);
static int   billingd_epoll_remove(int fd );
static int   billingd_epoll_add(int fd,int flag);
static int   billing_epoll_wait();
static void  handle_accept(int listen_fd);
static void  disconnect_fd(int fd);
static int   parse_billingd(char * buf,struct billing_data * billing_data);
static int   read_billing_info(int sock_fd);
static void  * writeFile(void * arg);

#endif
