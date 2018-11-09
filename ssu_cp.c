#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <pwd.h>
#include <utime.h>
#include <limits.h>
#include <time.h>

#define bool int
#define true 1
#define false 0

void error(char *name);// 해당 name을 내용으로 하는 에러 문장 출력
void judge(); // 폴더인지 파일인지 검사
void copy_file(char *s, char *t, struct stat src_st); // 파일 복사
void opt_p(struct stat); // p옵션
void copy_dir(char *s, char *t); // 폴더 복사
void symbol(char *s, char *t); // 심볼릭
int check_over(char *t); // 오류검사
char* get_realPath(char *sen); // 절대경로 리턴

char * target; // 타겟
char * source; // 소스
bool option[7] = {0}; // 옵션 
char optchar[7] = {'s', 'i', 'l', 'n', 'p', 'r', 'd'};
int multi; // 멀티프로세스 개수
pid_t *multi_process; // 프로세스 
pid_t mother;
int now_process; // 현재 폴더수
char *backup_src, *backup_trg;

int main(int argc, char* argv[]){
	if(argc>2)
	{
		if(argv[argc-2][0] == '-')
			error("usage : ./ssu_cp [option] [source] [target]");
	}
	else
		error("usage : ./ssu_cp [option] [source] [target]");

	int c;
	target = argv[argc-1]; // 인자로 들어오는 뒤에서 첫번째값은 target
	if(target[strlen(target)-1] == '\\'){ // \뒤에 있는 값들은 추가로 입력받는다.
		char hello[PATH_MAX] = {0};
		char *tt = (char*)malloc(PATH_MAX);
		printf("> ");
		scanf("%s", hello);
		strncpy(tt, target, strlen(target)-1);
		strcat(tt, hello);
		target = tt;
	}
	umask(0); // 권한설정 용이하기 위해 셋팅
	source = argv[argc-2]; // 인자로 들어오는 뒤에서 두번째값은 source
	backup_src = argv[argc-2]; // 백업시킴 (target source 출력하기 위해서)
	backup_trg = argv[argc-1];
	mother = getpid();
	while((c = getopt(argc, argv, "silnprd:")) != -1){
		switch(c){
			case 'S': case 's':
				if(option[0] == true) // 옵션 중복되면 에러처리
					error("-s option has already used\nYou must use same option once a time");
				option[0] = true;
				break;
			case 'I': case 'i':
				if(option[1] == true)
					error("-i option has already used\nYou must use same option once a time");
				option[1] = true;
				break;
			case 'L': case 'l':
				if(option[2] == true)
					error("-l option has already used\nYou must use same option once a time");
				option[2] = true;
				break;
			case 'n': case 'N':
				if(option[3] == true)
					error("-n option has already used\nYou must use same option once a time");
				option[3] = true;
				break;
			case 'p': case 'P':
				if(option[4] == true)
					error("-p option has already used\nYou must use same option once a time");
				option[4] = true;
				break;
			case 'r': case 'R':
				if(option[5] == true)
					error("-r option has already used\nYou must use same option once a time");
				option[5] = true;
				break;
			case 'd': case 'D':
				if(option[6] == true)
					error("-d option has already used\nYou must use same option once a time");
				option[6] = true;
				multi = atoi(optarg);
				if(multi == 0) // 뒤에 숫자가 안올경우
					error("[-d] option need to integer N (1 <= N <= 10)");
				if(multi >10 && multi < 1)
					error("-d [N] N is 1 <= N <= 10");
				multi_process = (pid_t*)malloc(sizeof(pid_t)*multi); // 멀티프로세스
				break;
		}
	}
	judge();
}

void error(char* name){
	printf("Error: %s\n", name); // 에러내용 출력하고
	printf("Usage : in case of file\n"); // 올바른 사용법 제시
	printf(" cp [-i/n][-l][-p] [source] [file]\n");
	printf(" or cp [-s] [source] [file]\n");
	printf(" in case of directory cp [-i/n][-l][-p][-r][-d][N]\n");
	exit(1); // 강제종료
}

void judge(){
	struct stat src_st;
	struct stat trg_st;
	stat(target, &trg_st);
	stat(source, &src_st);
	// 파일 상태 출력
	if(S_ISLNK(src_st.st_mode)){ // 링크면 원본 불러옴
		char * path = (char*)malloc(PATH_MAX);
		readlink(source, path, PATH_MAX);
		source = path;
		stat(source, &src_st); // 원본파일로 파일상태 갱신
	}

	if(option[0]) // s옵션은 단독으로만 쓰인다.
		for(int i = 1; i<7; i++)
			if(option[i])
				error("-s option must be used to itself only. [-s] [source] [target]");
	for(int i = 0; i<7; i++){
		if(option[i]) // 활성화된 옵션 파악
			printf(" %c option is on\n", optchar[i]);
	}
	printf("target : %s\nsrc : %s\n", target, source); // 타겟 소스 출력

	source = get_realPath(source); // 절대경로 출력
	target = get_realPath(target);
	
	if(src_st.st_ino == trg_st.st_ino) // 동일한 파일인지 확인
		error("[target] == [source]");
	
	if(S_ISREG(src_st.st_mode)){ // 파일이면
		if(option[5] || option[6]) // 폴더옵션 사용 못함
			error("[-r] [-d] option use only cp directory");
		if(option[1] && option[3])
			error("[-i] [-n] option can't use same time");
		if(option[4]) // p옵션
			opt_p(src_st);
		if(S_ISDIR(trg_st.st_mode)) // 파일 -> 폴더 안된다.
			error("Can't overwrite File to Directory");
		copy_file(source, target, src_st); // 파일복사
	}
	else if(S_ISDIR(src_st.st_mode)){ // 폴더복사
		if(!(option[5] || option[6])) // 폴더옵션 없으면 에러
			error("If you want to copy directory, you must use option [-r] or [-d][N].\n(You can't use both of them)");
		else if(option[5] && option[6])
			error("Can't use [-r] [-d][N] once a time");
		if(option[4]) // p옵션
			opt_p(src_st);
		if(S_ISREG(trg_st.st_mode))
			error("Can't overwrite Directory to File");
		copy_dir(source, target);
	}
	else{
		char error_print[PATH_MAX+29] = {0}; // 아무것도 없다!
		strcpy(error_print, backup_src);
		strcat(error_print, " : No Such file or directory");
		error(error_print);
	}
}

void opt_p(struct stat a){
	struct passwd *ps = getpwuid(a.st_uid);
	struct tm *tm;
	printf("*****************file info*****************\n");
	if(S_ISREG(a.st_mode)) // 파일인지 확인
		printf("파일이름 : %s\n", backup_src);
	else
		printf("폴더이름 : %s\n", backup_src);
	tm = localtime(&(a.st_atime));
	printf("마지막 읽은 시간 : %d.%d.%d %d:%d:%d\n", tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
	tm = localtime(&(a.st_mtime));
	printf("마지막 수정 시간 : %d.%d.%d %d:%d:%d\n", tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
	tm = localtime(&(a.st_ctime));
	printf("마지막 변경 시간 : %d.%d.%d %d:%d:%d\n", tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
	printf("OWNER : %s\n", ps->pw_name);
	ps = getpwuid(a.st_gid); // 파일 정보 불러옴
	printf("GROUP : %s\n", ps->pw_name);
	printf("file size : %ld\n", a.st_size);
}

void copy_file(char *s, char *t, struct stat src_st){
	int nfd;
	int fd;
	int over = 1;
	char *data = (char*)malloc(src_st.st_size);
	int length;
	struct utimbuf time_buf;
	struct stat trg_st;
	stat(t, &trg_st);
	if(option[0])
	{
		if(S_ISLNK(trg_st.st_mode))
			error("Target is already exist");
		symbol(s, t);
	}
	else{
		if(S_ISREG(trg_st.st_mode))
		{
			if(option[1]){
				if(check_over(t) == 0)
					return;
			}
			else if(option[3])
				return;
		}
		time_buf.actime = src_st.st_atime;
		time_buf.modtime = src_st.st_mtime;
		unlink(t);
		nfd = open(t, O_WRONLY|O_CREAT|O_TRUNC, 0775);
		fd = open(s, O_RDONLY);
		
		while((length = read(fd, data, src_st.st_size)) > 0)// 파일의 크기만큼 읽어온다.
			write(nfd, data,length); // 크기만큼 쓴다.
		
		close(fd);
		close(nfd);
		
		if(option[2]){
			utime(t, &time_buf);
			chown(t, src_st.st_uid, src_st.st_gid);
			chmod(t, src_st.st_mode);
		}
		free(data);
	}
}

void copy_dir(char *s, char *t){
	struct stat src_st;
	struct stat trg_st;
	stat(s, &src_st);
	stat(t, &trg_st);

	if(!S_ISDIR(trg_st.st_mode)) // 폴더가 존재하지 않는다.
		mkdir(t, 0775); // 만든다.
	chdir(s); // 해당 폴더로 이동
	struct dirent **namelist;
	int cont_cnt = scandir(s, &namelist, NULL, alphasort); // 해당폴더의 데이터들을 쫙 불러옴
	char path[PATH_MAX] = {0};
	strcpy(path, t); // 복사대상
	path[strlen(path)] = '/'; // 밑밥깔아둠
	for(int i = 0; i<cont_cnt ; i++){
		if((!strcmp(namelist[i]->d_name, ".") || !strcmp(namelist[i]->d_name, "..")))
			continue; // 넘기고!
		struct stat what;
		strncat(path, namelist[i]->d_name, strlen(namelist[i]->d_name)); // 파일 경로
		stat(namelist[i]->d_name, &what); // 해당 파일의 상태를 불러옴
		if(S_ISREG(what.st_mode)) // 파일이면
			copy_file(namelist[i]->d_name, path, what); // 복사한다!
		else{ // 폴더면
			char now[PATH_MAX] = {0};
			strcpy(now, s);
			now[strlen(now)] = '/';
			strncat(now, namelist[i]->d_name, strlen(namelist[i]->d_name)); // 파일 경로 확장
			if(mother == getpid() && option[6]) // multiprocess
			{
				if(now_process != multi){
					int sat;
					multi_process[now_process++] = fork(); // 프로세스 만든다.
					if(multi_process[now_process-1] == 0) // 자식프로세스!
					{
						printf("Folder = %s\nPID = %d\n", now, getpid());
						copy_dir(now, path); // 폴더복사 실행
					}
					else if(multi_process[now_process-1] > 0)
						wait(&sat);
				}
			}
			else
				copy_dir(now, path); // 아니면 그냥 복사한다.
		}
		for(int k = 0; k<strlen(namelist[i]->d_name) ; k++)
			path[strlen(path)-1] = '\0'; // 복사 후 다시 원래 경로로 되돌려놈
	}

	for(int i = 0; i<cont_cnt; i++)
		free(namelist[i]); // 네임리스트 리셋
	free(namelist);
	chdir(".."); // 이전폴더로 이동한다.
}

void symbol(char *s, char *t){ // 심볼릭 링크 생성
	symlink(s, t);
	return;
}

int check_over(char * t){ // 파일 중복되는지 확인한다.
	char jud = 0;		
	printf("overwrite %s (y/n)?\n", t);
	scanf(" %c", &jud);
	if(jud != 'y'){
		printf("cancel copy (overwrite stop)\n");
		return 0;
	}
	return 1;
}

char* get_realPath(char *sen){
	if(sen[0] != '/'){ // 상대경로일경우
		char *a = (char*)malloc(PATH_MAX);
		getcwd(a, PATH_MAX); // 현재경로 뽑아옴
		a[strlen(a)] = '/';
		strncat(a, sen, strlen(sen));
		return a;
	}
	return sen; // 절대경로면 그냥 그대로 리턴!
}
