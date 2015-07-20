#include "rtmpbillingd.h"

void show_help (void)
{
	printf("USAGE: billingd [options]\n"
		"OPTIONS:\n"
		"  -v      vision\n"
		"  -h/-?   help info\n"
		"  -c      To run the program: parameters: (1). Listen port of rtmpbillingd (2). the log saving interval(second) (3). The log saving directory (4). Pid file storage directory\n");
}

void options_parse(int argc, char **argv)
{
	int ch = 0;
	char chr = '/';

	while( (ch = getopt(argc, argv, "hvc:")) != -1 ) {

		switch(ch) {
			case '?':
			case 'h':
				show_help();
				exit(0);
			case 'v':
				fprintf(stderr, "VERSION: %s\n", VERSION);
				exit(0);
			case 'c':
				if(argc != 6) {
					
					return;
				}
				//port
				m_conf.port= atoi(optarg);
				//interval
				m_conf.interval = atoi(argv[3]);
				//log path
				memcpy(m_conf.path, argv[4], strlen(argv[4]));
				if (m_conf.path[strlen(argv[4]) -1] != chr) {
					
					m_conf.path[strlen(argv[4])] = '/';
					m_conf.path[strlen(argv[4]) + 1] = 0;
				}
				//pid log path
				memcpy(m_conf.pid_path, argv[5], strlen(argv[5]));
				if (m_conf.pid_path[strlen(argv[5]) -1] != chr) {
					
					m_conf.pid_path[strlen(argv[5])] = '/';
					m_conf.pid_path[strlen(argv[5]) + 1] = 0;
				}
				return;
				
			default:
				show_help();
				exit(0);
		}
	}
	
	show_help();
	exit(0);
}

int generate_pid_file(const char * pid_path)
{
	// open pid file    
	// -rw-r--r--
	// 644
	int fd = 0;
	if ((fd = open(pid_path, O_WRONLY | /*O_TRUNC*/O_CREAT, S_IRUSR | S_IWUSR |  S_IRGRP | S_IROTH)) < 0) {
		
		printf("open pid file %s error\n", pid_path);
		return -1;
	}
	// require write lock
	struct flock lock;

	lock.l_type = F_WRLCK; 		// F_RDLCK, F_WRLCK, F_UNLCK
	lock.l_start = 0; 			// type offset, relative to l_whence
	lock.l_whence = SEEK_SET;  	// SEEK_SET, SEEK_CUR, SEEK_END
	lock.l_len = 0;
	
	if (fcntl(fd, F_SETLK, &lock) < 0) {
		
		if(errno == EACCES || errno == EAGAIN) {
			
			printf("%s already running!\n", pid_path);
			return -1;
		}
		printf("require lock for file %s error!\n", pid_path);
		return -1;
	}
	// truncate file
	if (ftruncate(fd, 0) < 0) {
		
		printf("truncate pid file %s error!\n", pid_path);
		return -1;
	}
	
	int pid = (int)getpid();
	// write the pid
	char buf[128] = {0};
	snprintf(buf, sizeof(buf), "%d", pid);
	if (write(fd, buf, strlen(buf)) != (int)strlen(buf)) {
		
		printf("write our pid error! pid=%d file=%s\n", pid, pid_path);
		return -1;
	}
    fsync(fd);
	// auto close when fork child process.
	int val;
	if ((val = fcntl(fd, F_GETFD, 0)) < 0) {
		
		printf("fnctl F_GETFD error! file=%s\n", pid_path);
		return -1;
	}
	val |= FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, val) < 0) {
		
		printf("fcntl F_SETFD error! file=%s\n", pid_path);
		return -1;
	}

	printf("write pid=%d to %s success!\n", pid, pid_path);
	return 0;
}

int getdevice_name()
{
	FILE * fp = NULL;
	char device_name[MAX_FILE_NAME] = {0};
	char buf[MAX_FILE_NAME] = {0};
	int len  = 0 ;
    int i = 0; 

	// got sn_name
	if (NULL == (fp = fopen("/sn.txt", "r"))) {
		
		printf("fopen /sn.txt error\n");
		return -1;
	}
	while (fgets(buf, MAX_FILE_NAME, fp)) {
		
		if (device_name[0] == '#') {
			
			continue;
		}
		strcpy(device_name, buf);
		len = strlen(device_name);
		if (device_name[len-1] == '\n') {
			
			device_name[len-1] = 0;
		}
	}
	
	fclose(fp);
	memcpy(m_b_filename.device_name, device_name, len);
	
	for (i = 0 ;i< MAX_VHOST ; i++) {
		
		m_b_data[i].Timestamp  = 0;
		m_b_data[i].Interval = 0;
		memset(m_b_data[i].ChannelName, 0, MAX_CHANNELNAME);
		m_b_data[i].bytes_from_client = 0;
		m_b_data[i].bytes_to_client = 0;
	}
	return 1;
}

void signal_pro(int sig)
{
	if (m_epoll_fd >= 0) {

		billingd_epoll_remove(m_listen_fd);	
		close(m_listen_fd);
		close(m_epoll_fd);
	
		exit(0);
		printf("close epoll m_epoll_fd\n");
	}

	return; 
}
void create_socket()
{
	int socketfd;

	if ((socketfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		
		printf("socket creat error, exit\n");
		exit(0);
	}
	struct  sockaddr_in ServerAddress;
	memset(&(ServerAddress), 0, sizeof(ServerAddress));
	ServerAddress.sin_family = AF_INET;
	ServerAddress.sin_port  = htons(m_conf.port);
	ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);

	int option = 1;
	if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0) {
		
		printf("setsockopt error,exit\n");
		exit(0);
	}

	while (bind(socketfd,(struct sockaddr*)&ServerAddress, sizeof(struct sockaddr)) < 0) {
		
		printf("socket bind error\n");
		sleep(1);
		exit(0);
	}

	if (listen(socketfd, 10) < 0) {
		
		printf("socket listen error,exit\n");
		exit(0);
	}

	// add the socket to epoll to listen new connections
	billingd_epoll_add(socketfd, EPOLLIN | EPOLLHUP | EPOLLERR);
	m_listen_fd = socketfd;
};

void billingd_epoll_init(void)
{
	if((m_epoll_fd = epoll_create(MAX_EVENTS)) < 0) {
		
		printf("epoll creat error\n");
	}
	memset(m_events, 0, sizeof(m_events));
}

int billingd_epoll_remove(int fd)
{
	return epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int billingd_epoll_add(int fd,int flag)
{ 
	struct epoll_event cevent;
	memset(&cevent, 0, sizeof(struct epoll_event));
	cevent.events = flag;
	cevent.data.fd = fd;

	if(epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &cevent) == -1) {
		
		printf("epoll_add error\n");
		return -1;
	}
	return 0;
}

int billing_epoll_wait()
{
	int nfds;
	int i;
	int ret=0 ;

	nfds = epoll_wait(m_epoll_fd, m_events, MAX_EVENTS, -1);
	if(nfds == -1) {
		
		if(errno != EINTR ) {
			
			return -1;
		}

	} else if(0 == nfds) {
	
		return 0;
	}

	for (i = 0; i < nfds; i++) {
		
		if (m_listen_fd == m_events[i].data.fd) {
			
			handle_accept(m_listen_fd);
		}
		else if (m_events[i].events & EPOLLIN) {
			
			read_billing_info(m_events[i].data.fd);
			if (firstlogtime == 0) {
				
				firstlogtime = time(NULL);
				printf("firstlogtime : %d\n",firstlogtime);
			}
		}
		else if (m_events[i].events & (EPOLLHUP|EPOLLERR)) {
			
			disconnect_fd(m_events[i].data.fd);
		}
	}
	return ret;
}

void handle_accept(int listen_fd)
{
	struct sockaddr_in	client_addr;
	socklen_t len = sizeof(client_addr);
	int client_socket = 0;

	if ((client_socket = accept(listen_fd, (struct sockaddr*)&client_addr, &len)) < 0) {
		
		printf("accept error\n");
		return;
	}

	int opts;
	if ((opts = fcntl(client_socket, F_GETFL)) < 0) {
		
		goto failed;
	}

	opts = opts|O_NONBLOCK|FD_CLOEXEC;

	if (fcntl(client_socket, F_SETFL, opts) < 0) {
		
		goto failed;
	}

	if (billingd_epoll_add(client_socket, EPOLLIN | EPOLLHUP | EPOLLERR)) {
		
		goto failed;
	}
	return;
	
failed:
	printf("accept error,close client_sock\n");
	close(client_socket);
}

void disconnect_fd(int fd)
{
	billingd_epoll_remove(fd);
	close(fd);
}

int parse_billingd(char * buf, struct billing_data * billing_data)
{
	char* ps[MAX_FILE_NAME] = {0};
	int maxlen = 0;
	int index = 0;
	char* ps1, *ps2;
	int i = 0;
	ps1 = ps2 = buf;
	int bufsize = 0;

	while (1) {
		
		if (*ps1 == '\n') {
			
			break;
		}

		i = 0;
		while (*ps1 == '\t') {
			
			ps1++;
			ps2++;
			bufsize ++;
		}

		while (*ps2 != '\t' && *ps2 != '\n') {
			
			ps2++;
			i++;
		}
		if (i > maxlen) {
			
			maxlen = i;
		}
		ps[index] = (char *)malloc(sizeof(char)*(i + 1));

		strncpy(ps[index], ps1, i + 1);
		ps[index][i] = '\0';
		index++;
		while (ps1 < ps2) {
			
			ps1++;
			bufsize ++;
		}
	}
	if (index == 5) {
		
		billing_data->Timestamp = atoi(ps[0]);
		billing_data->Interval = atoi(ps[1]);
		memcpy(billing_data->ChannelName,ps[2],strlen(ps[2])+1);
		billing_data->bytes_from_client = strtoul(ps[3], 0, 10);
		billing_data->bytes_to_client = strtoul(ps[4], 0, 10);
	}
	
	for (i = 0;i< index;i++) {
		
		free(ps[i]);
	}
	return bufsize + 1;
}

int read_billing_info(int sock_fd)
{
	char tmp_buff[MAX_FILE_NAME] = {0};
	//char tmp_buff[MAX_FILE_NAME] = "1208760704 300 rtmp://vod.cctv.com  100000000 100000000";
	struct billing_data  billing_data;
	int i = 0;
	int len = read(sock_fd, tmp_buff, MAX_FILE_NAME);
	int bufsize = 0;
	int tmp_len = 0;
	if (len > 0) {
		
		while (len > 1) {
			
			bufsize = parse_billingd(tmp_buff + tmp_len,&billing_data);
			tmp_len += bufsize;
			len -= bufsize;
			printf("%d %d %s %lu %lu\n",
			billing_data.Timestamp,
			billing_data.Interval,
			billing_data.ChannelName,
			billing_data.bytes_from_client,
			billing_data.bytes_to_client);

			for (i = 0 ;i< MAX_VHOST ; i++) {
				
				if (m_b_data[i].Timestamp == 0) {
					
					m_b_data[i].Timestamp = billing_data.Timestamp;
					m_b_data[i].Interval = billing_data.Interval;
					memcpy(m_b_data[i].ChannelName,billing_data.ChannelName,strlen(billing_data.ChannelName));
					m_b_data[i].bytes_from_client = billing_data.bytes_from_client;
					m_b_data[i].bytes_to_client = billing_data.bytes_to_client;
					break;
				}
				if (!strcmp(billing_data.ChannelName, m_b_data[i].ChannelName)) {
					
					m_b_data[i].Timestamp = billing_data.Timestamp;
					m_b_data[i].Interval = billing_data.Interval;
					memcpy(m_b_data[i].ChannelName,billing_data.ChannelName,strlen(billing_data.ChannelName));
					m_b_data[i].bytes_from_client += billing_data.bytes_from_client;
					m_b_data[i].bytes_to_client += billing_data.bytes_to_client;
					break;
				}
			}
		}
	} else {
	
		disconnect_fd(sock_fd);
	}
	return 1;
}

void * writeFile(void * arg)
{
	if (m_b_data[0].Timestamp == 0) {
		
		return NULL;
	}

	struct billing_filename * b_filename = (struct billing_filename*)arg;
	FILE * billing_file = NULL;
	char billing_name[MAX_FILE_NAME]  ={0};
	char billing_content[MAX_BILLING_BUF] = {0};
	int i = 0;

	sprintf(billing_name,"%s%s_KSCLOUND_SML_%4d%02d%02d%02d%02d.log",
		m_conf.path,b_filename->device_name,
		b_filename->time->tm_year+1900,
		b_filename->time->tm_mon + 1,
		b_filename->time->tm_mday,
		b_filename->time->tm_hour,
		g_min);

	if ((billing_file = fopen(billing_name, "w")) == NULL ) {
		
		exit(1);
	}

	printf("write billing file billing_name : %s\n", billing_name);

	for (i = 0 ;i< MAX_VHOST ; i++) {
		
		if (m_b_data[i].ChannelName[0] != 0) {
			
			sprintf(billing_content, "%d\t%d\t%s\t%lu\t%lu\n",
				m_b_data[i].Timestamp,
				m_b_data[i].Interval,
				m_b_data[i].ChannelName,
				m_b_data[i].bytes_from_client,
				m_b_data[i].bytes_to_client);
			fwrite(billing_content, strlen(billing_content), 1, billing_file);

			m_b_data[i].Timestamp  = 0;
			m_b_data[i].Interval = 0;
			memset(m_b_data[i].ChannelName, 0, MAX_CHANNELNAME);
			m_b_data[i].bytes_from_client = 0;
			m_b_data[i].bytes_to_client = 0;
		}
	}
    fclose(billing_file);
	return NULL;
}

int main(int argc, char** argv)
{
	time_t first_timer;
	time_t second_timer;
	struct tm * tblock = NULL;
	char pidfile_name[MAX_PIDFILE_BUF]  ={0};
	int i, ret = 0;
	unsigned int intervel = 0;
	firstlogtime = 0;
	g_min = 0;

	options_parse(argc, argv);

	printf("==========billing_log start==============\n");

	sprintf(pidfile_name, "%srtmpbillingd.pid", m_conf.pid_path);
	ret = generate_pid_file(pidfile_name);
	if (ret < 0) {
		
		return 1;
	}

	ret = getdevice_name();
	if (ret < 0) {
		
		return 1;
	}
	
	billingd_epoll_init();

    create_socket();

	first_timer = time(NULL);
	printf("Localtime now : %d\n", first_timer);

	signal(SIGTERM, signal_pro);
	while (firstlogtime == 0) {
		
		billing_epoll_wait();
		tblock = localtime(&second_timer);
		m_b_filename.time = tblock;

		if (m_conf.interval < 60) {

			printf("error: intervel is litter than 1 minute");
			return 1;
		}
		intervel = m_conf.interval;
		intervel  = intervel/60;
		g_min = ((m_b_filename.time->tm_min)/intervel)*intervel;
		if(g_min >= 60) {  //if g_min is 60 minutes, we should set g_min to zero

			g_min = 0;
		}
			
		writeFile((void *)&m_b_filename);
		first_timer =  time(NULL);
		second_timer = time(NULL);
	}
	
	while (1) {
		
		second_timer = time(NULL);
		if (second_timer - first_timer > m_conf.interval) {
			
			second_timer -= 1;
			tblock = localtime(&second_timer);
			m_b_filename.time = tblock;
			g_min += intervel;
			if (g_min >= 60) {   //if g_min is 60 minutes, we shuld set g_min to zero

				g_min = 0;
			}
			writeFile((void *)&m_b_filename);
			first_timer = second_timer;
		}
		billing_epoll_wait();
	}
	return 1;
}
