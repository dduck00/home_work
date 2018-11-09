#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <time.h>
#include <wait.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#define bool int
#define true 1
#define false 0

int period;		// 주기
char *filename; // 절대경로
bool option[5]; // 옵션
int n_opt = 1;  // 파일의 개수 옵션이 활성화되며 ㄴ이 숫지가 바뀜
char *realnm;   // 파일 고유이름 argv[1]
int log_fd;		// 로그파일 디스크립터
pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
/*
0 = m ->백업대상이 수정된 경우에만 백업을 수행함 // 주기를 받는다. 하지만 주기마다 수정여부를 확인한다.
1 = n [N] -> 가장 최근 N개의 백업파일만 남긴다. 오래된 백업파일을 삭제한다. // 파일디스크립터 테이블을 리스트로 구현한다..
2 = d -> 디렉터리 백업 시행, 아닐경우 에러 쫌 복잡하다.
3 = c -> 파일이 있는지 비교해서 diff실행
4 = r -> 복구한다.
*/

typedef struct N
{ // 파일구조체 파일들을 리스트로 묶어서 쓰레드 구동함, 폴더 갱신시 비교용도로 data와 비교함
	struct N *next;
	pthread_t pid;
	char *data;
} NODE;

NODE *HEAD = NULL, *TAIL;

char *dir_list[1024];			 // 폴더리스트
int dir_cnt = 0;				 // 폴더갯수(서브디렉토리)
time_t dir_time_now[1024] = {0}; // 폴더별로 현재 mtime
time_t dir_time_new;			 //
int modi = 0;
char *ar;

char *convert(char *data);		// 16진수 문자열로 변환
void error(char *sen);			// 에러출력
void judge();					// 에러검사함수
void file_back(char *name);		// 실제로 파일을 복사함
int init_demon(void);			// 데몬프로세스 생성
bool check_process(char *name); // 데몬프로세스 생성시 기존에 생성된 프로세스가 있는지 확인함
char **find_in_folder(char *name, int *ret);
void do_back(char *name);									// 파일백업을 수행한다. m옵션, n옵션을 여기서 걸러준다. 쓰레드가 실행시킴
void recover();												// r옵션
char *deconvert(char *old);									// r옵션에서 뒤에 수정시간을 뽑아와야함
void compare();												// c옵션
void th(char *link);										// 쓰레드처리, 폴더관리함
void write_log(char *name, int size, time_t mt, int index); // backup_log를 작성함
void brid(void *arg);										// pthread_creat함수에서 쓰레드를 실행시키기위해 중간에 거쳐가는 역할
NODE *make_NODE(char *data);								// check_list를 통해 받은 결과로 노드를 만들지 말지 결정함
int check_list(char *data);									// 파일리스트들에 data와 동알힌 파일이 있는지 확인함
void siganl_handler(int signo);								// 시그널 핸들러, SIGUSR1이 날아오면 로그에 작성하고 끝냄

int main(int argc, char *argv[])
{
	if (argc < 3)
		error("<실행> <파일이름> <주기> (옵션)");
	char opt;
	filename = (char *)malloc(PATH_MAX);
	realpath(argv[1], filename);
	umask(0);
	realnm = (char *)malloc(strlen(argv[1]));
	strcpy(realnm, argv[1]);
	ar = (char *)malloc(strlen(argv[0]) + 1);
	strcpy(ar, argv[0]);
	if (access(argv[1], F_OK) != 0)
		error("파일이 존재하지 않습니다.");
	while ((opt = getopt(argc, argv, "mn:dcr")) != -1)
	{
		switch (opt)
		{
		case 'm':
			option[0] = true;
			break;
		case 'n':
			for (int i = 0; i < strlen(optarg); i++)
				if (optarg[i] < '0' || optarg[i] > '9')
					error("number is wrong");
			option[1] = true;
			n_opt = atoi(optarg);
			for (int i = 0; i < strlen(optarg); i++)
			{
				if (!(optarg[i] >= '0' && optarg[i] <= '9'))
					error("n옵션 뒤에 인자로 숫자가 나와야 합니다.");
			}
			if (optarg == NULL)
				error("n옵션 뒤에 인자로 숫자가 나와야 합니다.");
			break;
		case 'd':
			option[2] = true;
			break;
		case 'c':
			option[3] = true;
			break;
		case 'r':
			option[4] = true;
			break;
		}
	}
	period = atoi(argv[argc - 1]);

	if (option[4] == false && option[3] == false)
		if (period > 10 || period < 2)
			error("주기는 3~10");

	judge(); // 옵션판단, 에러출력 후 데몬 생성여부 판단.

	if (access("backup_log", F_OK) != 0) // 로그파일이 없다.
		log_fd = open("backup_log", O_CREAT | O_TRUNC | O_WRONLY, 0777);
	else
		log_fd = open("backup_log", O_WRONLY | O_APPEND); // 로그파일이 있다.

	chdir("backup"); // 백업폴더로 이동
	if (option[3] || option[4])
	{
		if (option[4])
			recover();
		if (option[3])
			compare();
	}
	else if (option[2]) // 쓰레드
	{
		struct stat st;
		time_t now = 0, new = 0;
		while (1)
		{
			if (access(filename, F_OK) != 0) // 폴더가 있는가.
			{
				write_log(filename, 0, 0, 3); // 없다.
				exit(0);
			}
			stat(filename, &st);
			new = st.st_mtime; // 변경시간 불러옴
			if (new != now) // 다르다 변경됨 혹은 처음시작
			{
				th(filename); // 해당폴더 백업
				now = new;
			}

			for (int k = 0; k < dir_cnt; k++)
			{
				if (access(dir_list[k], F_OK) == 0) // 폴더가 존재하는가.
				{
					stat(dir_list[k], &st); // 폴더의 시간을 뽑아옴
					dir_time_new = st.st_mtime;
					if (dir_time_now[k] != dir_time_new) // 해당폴더가 수정됨
					{
						th(dir_list[k]); // 해당폴더 백업실시(갱신)
						dir_time_now[k] = dir_time_new;
					}
				}
			}
			modi = 1;
		}
	}
	else
		do_back(filename); // 보통 복사 시행
}

void error(char *sen)
{
	syslog(LOG_ERR, "Error : %s", sen); // 에러출력(로그에)
	exit(1);
}

char *convert(char *data)
{
	char date[12] = {0};
	time_t timer;
	struct tm *t;

	timer = time(NULL);	// 현재 시각을 초 단위로 얻기
	t = localtime(&timer); // 초 단위의 시간을 분리하여 구조체에 넣기
	char *result = (char *)calloc(sizeof(char), PATH_MAX);
	if (strlen(data) > 255)
		error("경로의 길이가 255를 넘습니다.");
	for (int i = 0; i < strlen(data); i++)
	{
		char temp[3];
		sprintf(temp, "%x", data[i]);
		strcat(result, temp);
	}
	sprintf(date, "_%02d%02d%02d%02d%02d", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
	strcat(result, date);

	return result;
}

void judge()
{
	struct stat st;
	stat(filename, &st);

	if (access("backup", F_OK)) // 백업폴더가 없으면 생성
		mkdir("backup", 0777);

	if (S_ISDIR(st.st_mode))
		if (!option[2])
			error("If you backup directory, you must use option '-d'"); // 폴더인데 d옵션이 아님

	if (option[3] || option[4])
	{
		int cnt = 0;
		for (int i = 0; i < 5; i++)
		{
			cnt += option[i];
		}
		if (cnt != 1)
			error("c option and r option must use itself");
	}

	int jud = check_process(ar + 2); // 프로세스가 있는지 확인
	if (jud && jud != getpid())
	{
		printf("Send signal to %s process<%d>\n", ar, jud); // 제거한다!
		kill(jud, SIGUSR1);
	}

	if (!(option[3] || option[4])) // r과 d가 아니면 디몬프로세스를 만듬
		init_demon();
}

void do_back(char *name)
{ // 쓰레드가 이걸 실행시킴
	struct stat src_sc;
	time_t now = 0, new = 0;
	int i, j;
	int check = 0;
	while (1)
	{
		sleep(period); // 주기만큼 쉬어준다.
		stat(name, &src_sc);
		new = src_sc.st_mtime;				 // 현재 mtime입력받음
		if (option[0] == true && new == now) // m옵션 활성화, 시간 안변화함
			continue;
		if (option[1] == true) // n옵션 활성화
		{
			int cnt = 0;
			char **list = find_in_folder(name, &cnt); // 폴더에서 리스트 뽑아옴
			struct stat lis;
			for (int zz = 0; zz <= cnt - n_opt; zz++)
			{
				stat(list[zz], &lis);
				write_log(name, lis.st_size, lis.st_ctime, 4); // 삭제로그 입력
				unlink(list[zz]); // 삭제
			}
			for (j = 0; j < cnt; j++)
				free(list[j]);
			free(list);
		}

		if (access(name, F_OK) != 0) // 파일이 접근불가능
		{
			write_log(name, 0, new, 3); // 파일이 제거됨을 표시함
			if (option[2]) // 쓰레드일때
				pthread_exit(0); // 쓰레드 종료
			else
				exit(0); // 프로세스 종료처리
		}
		if (now == 0 || now == new)
			write_log(name, src_sc.st_size, new, 1); // 파일 백업시작
		else
			write_log(name, src_sc.st_size, new, 2); // 파일이 수정됨

		file_back(name); // 파일백업시작

		now = new;
	}
}

void file_back(char *name)
{
	int length;
	int fd;
	int nfd;
	char *data;
	struct stat src_st;
	fd = open(name, O_RDONLY);
	nfd = open(convert(name), O_CREAT | O_TRUNC | O_WRONLY, 0666); // 16진수로 변환한
	stat(name, &src_st);
	data = (char *)malloc(src_st.st_size);
	while ((length = read(fd, data, src_st.st_size)) > 0)
		write(nfd, data, length);
	close(fd);
	close(nfd);
	free(data);
}

int init_demon(void)
{

	pid_t pid;
	int fd, maxfd;

	pid = fork();

	if (pid != 0)
		exit(0);

	pid = getpid();
	printf("Daemon process initialization.\n");
	setsid();
	maxfd = getdtablesize();
	for (fd = 0; fd < maxfd; fd++)
		close(fd);
	signal(SIGUSR1, siganl_handler);
	fd = open("/dev/null", O_RDWR);
	dup(0);
	dup(0);
	return 0;
}

bool check_process(char *name)
{

	struct dirent **namelist;
	int cont_cnt = scandir("/proc", &namelist, NULL, alphasort); // 프로세스는 /proc폴더에 있음

	for (int i = 0; i < cont_cnt; i++)
	{
		if ((!strcmp(namelist[i]->d_name, ".") || !strcmp(namelist[i]->d_name, "..")))
			continue; // 넘기고!

		char loc[255];
		char buf[255] = {0};
		int fd;

		sprintf(loc, "/proc/%s/status", namelist[i]->d_name); // 프로세스의 아이디는 status파일 첫줄에 저장됨
		fd = open(loc, O_RDONLY);
		read(fd, buf, 254);
		if (strstr(buf, name) != NULL)
		{
			int back = atoi(namelist[i]->d_name);
			for (int j = 0; j < cont_cnt; j++)
				free(namelist[j]); // 네임리스트 리셋
			free(namelist);
			return back; // 프로세스 찾음
		}
	}
	for (int i = 0; i < cont_cnt; i++)
		free(namelist[i]); // 네임리스트 리셋
	free(namelist);

	return 0; // 못찾음
}

char **find_in_folder(char *name, int *ret) // 폴더에 해당 이름을 가진 파일이 있는지 확인
{

	char *real = convert(name); // 16진수로 변환
	int length = strlen(real);
	struct dirent **namelist;
	int cnt = scandir(".", &namelist, NULL, alphasort);
	char **list = (char **)malloc(cnt * sizeof(char *));
	int many = 0;
	for (int i = 0; i < cnt; i++)
	{
		if ((!strcmp(namelist[i]->d_name, ".") || !strcmp(namelist[i]->d_name, "..")))
			continue; // 넘기고!
		if (strlen(namelist[i]->d_name) != length)
			continue;

		for (int j = 0; j < length; j++)
		{
			if (real[j] == '_') // _만났으면 앞에 데이터가 똑같다 -> 이름이 같다.
			{
				list[many] = (char *)malloc(sizeof(char) * strlen(namelist[i]->d_name)); // 리스트에 추가
				strcpy(list[many], namelist[i]->d_name);
				many++;
				break;
			}
			if (real[j] != namelist[i]->d_name[j])
				break;
		}
	}
	for (int i = 0; i < cnt; i++)
		free(namelist[i]); // 네임리스트 리셋
	free(namelist);
	*ret = many; // call by reference로 수 저장
	return list; // 리스트 리턴
}

void compare()
{
	int ret;
	char **file_list = find_in_folder(filename, &ret);
	if (ret == 0)
	{
		printf("there is no backup file! <%s>", realnm);
		exit(0);
	}
	int select;
	printf("<Compare with backup file[%s%s]>\n", realnm, deconvert(file_list[ret - 1])); //가장 최근파일과 비교함
	pid_t pid;
	if ((pid = fork()) == 0)
	{
		execl("/usr/bin/diff", "diff", filename, file_list[ret - 1], NULL); // 비교한다.
		exit(0);
	}
	else
		wait(0);
}

void recover()
{
	int ret;
	chdir("backup");								   // 백업폴더로 이동.
	char **file_list = find_in_folder(filename, &ret); // 해당 파일 리스트 불러옴
	int select;
	if (ret == 0)
	{
		printf("there is no backup file! <%s>", realnm);
		exit(0);
	}
	printf("<%s backup list>\n", realnm);
	printf("0 : exit\n");
	for (int i = 0; i < ret; i++)
		printf("%d : %s%s\n", i + 1, realnm, deconvert(file_list[i])); // 출력

	printf("input : ");

	scanf("%d", &select);
	if (select == 0 || select > ret) // 해당 숫자보다 많거나 0을 입력
	{
		printf("Cancel recover\n");
		exit(0);
	}
	printf("Recovery backup file...\n");
	printf("[%s]\n", realnm);
	fflush(stdout);
	unlink(filename); // 원본 삭제
	rename(file_list[select - 1], filename); // 복사한다.
	pid_t pid;
	if ((pid = fork()) == 0)
	{
		execl("/bin/cat", "cat", filename, NULL); // 내용물 출력
		exit(0);
	}
	else
		wait(0);
}

char *deconvert(char *old)
{
	int i;
	for (i = 0; old[i] != '_'; i++);

	return old + i; // 뒤에 숫자만 뽑아옴
}

void th(char *link)
{
	struct dirent **namelist;
	int cont_cnt = scandir(link, &namelist, NULL, alphasort);

	for (int i = 0; i < cont_cnt; i++)
	{
		if ((!strcmp(namelist[i]->d_name, ".") || !strcmp(namelist[i]->d_name, "..")))
			continue; // 넘기고!

		char *resource = (char *)malloc(PATH_MAX);
		struct stat src;
		sprintf(resource, "%s/%s", link, namelist[i]->d_name);
		stat(resource, &src);

		if (S_ISDIR(src.st_mode)) // 서브디렉터리
		{
			int k;
			for (k = 0; k < dir_cnt; k++)
			{
				if (strcmp(dir_list[k], resource) == 0) // 기존에 있는 디렉터리인지 확인
					break;
			}
			if (k == dir_cnt) // 아니면 새로운 디렉터리 추가함
			{
				dir_list[k] = resource;
				dir_cnt++;
			}
			th(resource); // 디렉터리 백업 시행
		}
		else
		{
			// 리스트에 넣어주기 전에 겹치는 파일 있는지 검사할 것 strcmp로 비교
			if (check_list(resource))
			{
				if (HEAD == NULL)
				{
					HEAD = make_NODE(resource);
					TAIL = HEAD;
				}
				else
				{
					TAIL->next = make_NODE(resource);
					TAIL = TAIL->next;
				}
			}
		}
	}
	for (int i = 0; i < cont_cnt; i++)
		free(namelist[i]); // 네임리스트 리셋
	free(namelist);

	return;
}

void write_log(char *name, int size, time_t mt, int index)
{
	char *melong = (char *)malloc(PATH_MAX);
	time_t timer = time(NULL);		  // 현재 시각을 초 단위로 얻기
	struct tm *t = localtime(&timer); // 초 단위의 시간을 분리하여 구조체에 넣기
	char temp[14];
	sprintf(temp, "%02d%02d %02d:%02d:%02d", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
	struct tm *tt = localtime(&mt);
	int k;

	for (k = strlen(name) - 1; k >= 0; k--)
	{
		if (name[k] == '/')
			break;
	}
	k++;

	if(modi == 1 && index == 1)
		index = 2;
	switch (index)
	{
	case 1:
		sprintf(melong, "[%s] %s [size:%d/mtime:%02d%02d %02d:%02d:%02d]\n", temp, name + k, size, tt->tm_mon + 1, tt->tm_mday, tt->tm_hour, tt->tm_min, tt->tm_sec);
		break;
	case 2:
		sprintf(melong, "[%s] %s is modified [size:%d/mtime:%02d%02d %02d:%02d:%02d]\n", temp, name + k, size, tt->tm_mon + 1, tt->tm_mday, tt->tm_hour, tt->tm_min, tt->tm_sec);
		break;
	case 3:
		sprintf(melong, "[%s] %s is deleted\n", temp, name + k);
		break;
	case 4:
		sprintf(melong, "[%s] Delete backup [%s, size:%d, btime:%02d%02d %02d:%02d:%02d]\n", temp, name + k, size, tt->tm_mon + 1, tt->tm_mday, tt->tm_hour, tt->tm_min, tt->tm_sec);
		break;
	}
	if (option[2]) // 쓰레드에서 파일디스크립터를 경쟁적으로 사용하는건 write할때뿐이다. 따라서 mutex로 묶어줌
	{
		pthread_mutex_lock(&mut);
		write(log_fd, melong, strlen(melong));
		pthread_mutex_unlock(&mut);
	}
	else
		write(log_fd, melong, strlen(melong));
}

void brid(void *arg)
{
	char *re = (char *)arg; // 변환
	do_back(re); // 실행
}

NODE *make_NODE(char *data)
{
	NODE *temp = (NODE *)malloc(sizeof(NODE));
	temp->data = data; // 동적할당된 데이터이니 만큼 그냥 찍어주면된다.
	temp->next = NULL;
	pthread_create(&(temp->pid), NULL, (void *)(&brid), (void *)data); // 쓰레드를 생성
	return temp;
}

int check_list(char *data)
{
	NODE *now;
	for (now = HEAD; now != NULL; now = now->next) // 파일리스트에서 해당 파일이 있는지 찾아봄
	{
		if (strcmp(now->data, data) == 0)
			return 0; // 있다.
	}
	return 1; // 없다.
}

void siganl_handler(int signo)
{
	char melong[PATH_MAX] = {0};
	time_t timer = time(NULL);																													   // 현재 시각을 초 단위로 얻기
	struct tm *t = localtime(&timer);																											   // 초 단위의 시간을 분리하여 구조체에 넣기
	sprintf(melong, "[%02d%02d %02d:%02d:%02d] %s<pid:%d> exit\n", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, ar + 2, getpid()); // 로그에 출력한다.
	pthread_mutex_lock(&mut);
	write(log_fd, melong, strlen(melong));
	pthread_mutex_unlock(&mut);
	exit(0);
}