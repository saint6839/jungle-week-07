#include <stdio.h>
#include "csapp.h"

#define WEBSERVER_HOST "localhost"
#define WEBSERVER_PORT 8080

/* cache */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_SIZE 10

/* thread pool */
#define NTHREADS 10
#define MAXQUEUE 100

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *request_line_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *end_of_hdr = "\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";

static const char *host_header = "Host";
static const char *connection_header = "Connection";
static const char *proxy_connection_header = "Proxy-Connection";
static const char *user_agent_header = "User-Agent";

void *worker_thread(void *arg);
void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio);
int connect_webserver(char *hostname, int port, char *http_header);

/* caching function */
void cache_init();
int cache_find(char *uri);
void cache_uri(char *uri, char *response_buf);
void get_cache_lock(int index);
void put_cache_lock(int index);

typedef struct
{
  char cache_obj[MAX_OBJECT_SIZE];
  char cache_uri[MAXLINE];
  int eviction_priority; // LRU 알고리즘에 의한 소거 우선순위. 숫자가 작을수록 소거에 대한 우선 순위가 높아짐
  int is_empty;          // 이 블럭에 캐시 정보가 들었는지 empty인지 아닌지 체크

  int reader_count; // cache block에 접근중인 reader 수
  sem_t wmutex;     // cache block 쓰기 lock 여부
  sem_t rdcntmutex; // cache block 읽기 lock 여부
} cache_block;

typedef struct
{
  cache_block cache_blocks[CACHE_SIZE];
} Cache;

/* cache 전역 변수 */
Cache cache;

/* worker thread 에게 넘길 인자 구조체 */
typedef struct
{
  int connfd;
  struct sockaddr_storage clientaddr;
  socklen_t clientlen;
} thread_args;

/* thread pool 전역 변수 */
pthread_t thread_pool[NTHREADS];                           // worker thread 수
thread_args queue[MAXQUEUE];                               // worker thread 에게 넘길 인자 배열
int queue_head = 0;                                        // 큐에서 제거할 위치
int queue_tail = 0;                                        // 큐에서 삽입할 위치
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;   // 큐 독점 액세스 보장
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER; // 큐에 작업이 있고, worker thread에서 작업을 처리할 수 있을때 worker thread에게 알리는 변수
pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;  // 큐에 여유 공간이 있어서, 작업을 더 받을 수 있음을 알림

int main(int argc, char **argv)
{
  int listenfd;
  char hostname[MAXLINE], port[MAXLINE];
  struct sockaddr_storage clientaddr;
  socklen_t clientlen;

  cache_init();

  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  // 프로세스가 닫히거나 끊어진 파이프에 쓰기 요청을 할 경우 발생하는 오류(SIGPIPE)를 무시하고 서버를 계속 동작시킬 수 있도록 처리
  Signal(SIGPIPE, SIG_IGN);

  listenfd = Open_listenfd(argv[1]);

  /* thread pool 초기화 */
  for (int i = 0; i < NTHREADS; i++)
  {
    Pthread_create(&thread_pool[i], NULL, worker_thread, NULL);
  }

  while (1)
  {
    clientlen = sizeof(clientaddr);
    int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s %s).\n", hostname, port);

    pthread_mutex_lock(&queue_mutex); // connection에 대한 작업을 큐에 쓰기 위해서 큐에 대한 access lock 획득

    /* 큐가 가득 찼을 경우 worker thread를 queue_not_full 신호를 보내기 전까지 wait 시킴 */
    while ((queue_tail + 1) % MAXQUEUE == queue_head)
    {
      pthread_cond_wait(&queue_not_full, &queue_mutex);
    }

    /* worker thread가 처리할 인자를 포함한 구조체 초기화 */
    queue[queue_tail].connfd = connfd;
    queue[queue_tail].clientaddr = clientaddr;
    queue[queue_tail].clientlen = clientlen;
    queue_tail = (queue_tail + 1) % MAXQUEUE; // 큐의 다음 작업 삽입 위치

    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
  }

  return 0;
}

void *worker_thread(void *arg)
{
  while (1)
  {
    pthread_mutex_lock(&queue_mutex); // 동시성 제어 하기 위해 진입시 lock 획득

    /* 작업 queue가 비어있는 동안 */
    while (queue_head == queue_tail)
    {
      /* queue_not_empty 신호를 받을때까지, queue_mutex를 이용해서 큐에 대한 접근을 lock하고 대기 */
      pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }

    thread_args t_args = queue[queue_head];   // head 위치에서 다음 작업 연결 세부 정보 가져옴
    queue_head = (queue_head + 1) % MAXQUEUE; // head 포인터를 증가 시켜 다음 uri작업 대상을 가리킴

    pthread_cond_signal(&queue_not_full); // 큐에 공간이 있음을 queue_not_full 조건 변수에 신호 보냄
    pthread_mutex_unlock(&queue_mutex);   // 큐의 동시성과 관련된 로직 종료되었으므로 lock 반환

    doit(t_args.connfd);
    Close(t_args.connfd);
  }
}

void doit(int connfd)
{
  int web_connfd, port;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char webserver_http_header[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE];

  rio_t rio, server_rio;

  Rio_readinitb(&rio, connfd);
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers: \n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version); // 클라이언트 reqeust line 정보 파싱 {method} {path or uri} {http version}

  if (strcasecmp(method, "GET"))
  {
    printf("Proxy does not implement the method");
    return;
  }

  /* 요청 uri 주소가 캐싱되어 있는 주소 인지 */
  int cache_index;
  if ((cache_index = cache_find(uri)) != -1)
  {
    get_cache_lock(cache_index); // cache block lock 획득
    // 캐시에서 찾은 값을 connfd에 쓰고, 캐시에서 그 값을 바로 보내게 됨
    cache.cache_blocks[cache_index].eviction_priority = CACHE_SIZE;                                                   // 가장 최근 읽혔으므로, 소거 우선 순위 가장 낮게
    update_cache_eviction_priority(cache_index);                                                                      // 나머지 cache block 소거 우선 순위 증가
    Rio_writen(connfd, cache.cache_blocks[cache_index].cache_obj, strlen(cache.cache_blocks[cache_index].cache_obj)); // 클라이언트에게 캐싱 데이터 응답
    put_cache_lock(cache_index);                                                                                      // cache block lock 반환
    return;
  }

  char uri_copy[1000];
  strcpy(uri_copy, uri);
  parse_uri(uri, hostname, path, &port);                          // uri 로부터 hostname, path, port 파싱하여 변수에 할당
  build_http_header(webserver_http_header, hostname, path, &rio); // hostname, path, port와 클라이언트 요청을 기반으로 웹 서버에 전송할 요청 헤더 재구성

  web_connfd = connect_webserver(hostname, port, webserver_http_header); // 소켓 생성, 웹 서버와 연결
  if (web_connfd < 0)
  {
    printf("connection failed\n");
    return;
  }

  Rio_readinitb(&server_rio, web_connfd);
  Rio_writen(web_connfd, webserver_http_header, strlen(webserver_http_header)); // 웹 서버로 재구성한 요청 헤더를 전송

  char response_buf[MAX_OBJECT_SIZE];
  int size_buf = 0;
  size_t n;

  /* 웹 서버 응답을 한 줄씩 읽어서 클라이언트에게 전달 */
  while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
  {
    // printf("proxy received %ld bytes, then send\n", n);
    size_buf += n;
    /* proxy거쳐서 서버에서 response오는데, 그 응답을 저장하고 클라이언트에 보냄 */
    if (size_buf < MAX_OBJECT_SIZE) // response_buf에 제한 두지 않고 계속 쓰다보면 buffer overflow 발생
      strcat(response_buf, buf);
    Rio_writen(connfd, buf, n);
  }

  Close(web_connfd);

  /* 저장된 response_buf의 크기가 cache block에 저장될 수 있는 최대 크기보다 작을때만 캐싱 */
  if (size_buf < MAX_OBJECT_SIZE)
    cache_uri(uri_copy, response_buf);
}

void build_http_header(char *http_header, char *hostname, char *path,
                       rio_t *client_rio)
{
  char buf[MAXLINE], request_line[MAXLINE], other_hdr[MAXLINE],
      host_hdr[MAXLINE];
  /* request line 생성 */
  sprintf(request_line, request_line_hdr_format, path);

  /* 클라이언트 입력 스트림 버퍼를 한 줄씩 읽어서 HTTP header를 만듦 */
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0)
  {
    if (strcmp(buf, end_of_hdr) == 0)
      break;

    /* 대소문자 여부 상관 없이 비교 if true -> return 0 */
    if (!strncasecmp(buf, host_header, strlen(host_header)))
    {
      strcpy(host_hdr, buf);
      continue;
    }

    /* 기타 헤더 정보 */
    if (!strncasecmp(buf, connection_header, strlen(connection_header)) &&
        !strncasecmp(buf, proxy_connection_header, strlen(proxy_connection_header)) &&
        !strncasecmp(buf, user_agent_header, strlen(user_agent_header)))
    {
      strcat(other_hdr, buf);
    }
  }
  if (strlen(host_hdr) == 0)
    sprintf(host_hdr, host_hdr_format, hostname);
  sprintf(http_header, "%s%s%s%s%s%s%s", request_line, host_hdr, conn_hdr,
          prox_hdr, user_agent_hdr, other_hdr, end_of_hdr);
  printf("%s\n", http_header);

  return;
}
int connect_webserver(char *hostname, int port, char *http_header)
{
  char port_str[100];
  sprintf(port_str, "%d", port);
  return Open_clientfd(hostname, port_str);
}

/* 요청된 uri로부터 hostname, path, port를 parsing */
void parse_uri(char *uri, char *hostname, char *path, int *port)
{
  /* default webserver host, port */
  strcpy(hostname, WEBSERVER_HOST);
  *port = WEBSERVER_PORT;

  /* http:// 이후의 host:port/path parsing */
  char *pos = strstr(uri, "//");
  pos = pos != NULL ? pos + 2 : uri;

  /* host: 이후의 port/path parsing*/
  char *pos2 = strstr(pos, ":");

  /* port 번호를 포함하여 요청했다면  */
  if (pos2 != NULL)
  {
    *pos2 = '\0';
    sscanf(pos2 + 1, "%d%s", port, path); // 숫자는 port에 이후 문자열은 path에 저장
  }
  else /* port 번호가 없이 요청 왔다면 */
  {
    pos2 = strstr(pos, "/");
    if (pos2 != NULL) // path를 통해 특정 자원에 대한 요청이 있을 경우
    {
      sscanf(pos2, "%s", path); // pos2 위치의 문자열을 path에 저장함
    }
  }
  return;
}

/* 캐시 초기화 함수 */
void cache_init()
{
  for (int i = 0; i < CACHE_SIZE; i++)
  {
    cache.cache_blocks[i].eviction_priority = 0; // 아직 캐싱된 데이터 없으므로 0, 최근에 할당 된 cache block 일 수록 높은 값을 가짐
    cache.cache_blocks[i].is_empty = 1;          // 아직 캐싱된 데이터 없으므로 1

    // 두번째 파라미터 1이면 process shared, 0이면 thread shared, 세번째 파라미터 -> 세마포어 초기값 1(액세스 가능)
    Sem_init(&cache.cache_blocks[i].wmutex, 0, 1);     // -> 진입 가능한 자원 1개뿐이므로 binary semaphore
    Sem_init(&cache.cache_blocks[i].rdcntmutex, 0, 1); // -> 진입 가능한 자원 1개뿐이므로 binary semaphore
    cache.cache_blocks[i].reader_count = 0;
  }
}

/* cache block 접근 전 cache block access lock을 얻기 위한 함수 */
void get_cache_lock(int index)
{
  P(&cache.cache_blocks[index].rdcntmutex); // reader count 값 변경에 대한 lock 획득

  cache.cache_blocks[index].reader_count++;        // read count 증가(조회 하러 들어가므로)
  if (cache.cache_blocks[index].reader_count == 1) // reader_count == 1 -> 현재 읽는 사용자 한명만 캐시 블록에 접근 중
    P(&cache.cache_blocks[index].wmutex);          // cache block에 대한 write lock 획득

  V(&cache.cache_blocks[index].rdcntmutex); // reader count 값 변경에 대한 lock 반환
}

/* cache block 접근 끝난 이후 cache block access lock을 반환하기 위한 함수 */
void put_cache_lock(int index)
{
  P(&cache.cache_blocks[index].rdcntmutex); // reader count 값 변경에 대한 lock 획득

  cache.cache_blocks[index].reader_count--;        // read count 감소(조회 끝났으므로)
  if (cache.cache_blocks[index].reader_count == 0) // reader_count == 0 -> 현재 읽는 사용자 한명만 캐시 블록에 접근 중
    V(&cache.cache_blocks[index].wmutex);          // cache block에 대한 write lock 획득

  V(&cache.cache_blocks[index].rdcntmutex); // reader count 값 변경에 대한 lock 획득
}

/* cache 에서 요청 uri와 일치하는 uri를 가지고 있는 cache block을 탐색하여 해당 block의 index 반환 */
int cache_find(char *uri)
{
  printf("\ncache hit ! ====> %s\n", uri);
  for (int i = 0; i < CACHE_SIZE; i++)
  {
    get_cache_lock(i);
    /* cache block 이 empty 가 아니고, cache block에 있는 uri이 현재 요청 uri과 일치한다면 cache block의 index 반환 */
    if (strcmp(uri, cache.cache_blocks[i].cache_uri) == 0)
    {
      put_cache_lock(i);
      return i;
    }
    put_cache_lock(i);
  }
  return -1;
}

/* eviction_priority 알고리즘에 따라 최소 eviction_priority 값을 갖는 cache block의 index 찾아 반환 */
int cache_eviction()
{
  int min = CACHE_SIZE;
  int minindex = 0;
  for (int i = 0; i < CACHE_SIZE; i++)
  {
    get_cache_lock(i);
    /* cache block empty 라면 해당 block의 index를 반환 */
    if (cache.cache_blocks[i].is_empty == 1)
    {
      put_cache_lock(i);
      return i;
    }
    /* eviction_priority가 현재 최솟값 min 보다 작다면 eviction_priority 값을 갱신 해주면서 최소 cache block 탐색*/
    if (cache.cache_blocks[i].eviction_priority < min)
    {
      minindex = i;                                  // i로 minindex 갱신
      min = cache.cache_blocks[i].eviction_priority; // min은 i번째 cache block의 eviction_priority 값으로 갱신
    }
    put_cache_lock(i);
  }
  return minindex;
}

void update_cache_eviction_priority(int index)
{
  for (int i = 0; i < CACHE_SIZE; i++)
  {
    if (i == index)
      continue;

    P(&cache.cache_blocks[i].wmutex); // cache block 쓰기 lock 획득

    if (cache.cache_blocks[i].is_empty == 0)
      cache.cache_blocks[i].eviction_priority--; // 최근 캐싱된 cache block을 제외한 나머지 cache block eviction_priority 값을 감소 시킴

    V(&cache.cache_blocks[i].wmutex); // cache block 쓰기 lock 반환
  }
}

/* empty cache block에 uri 캐싱 */
void cache_uri(char *uri, char *response_buf)
{
  int index = cache_eviction(); // 빈 캐시 블럭을 찾는 첫번째 index

  P(&cache.cache_blocks[index].wmutex); // cache block 쓰기 lock 획득

  strcpy(cache.cache_blocks[index].cache_obj, response_buf); // 웹 서버 응답 값을 캐시 블록에 저장
  strcpy(cache.cache_blocks[index].cache_uri, uri);          // 클라이언트의 요청 uri를 캐시 블록에 저장
  cache.cache_blocks[index].is_empty = 0;                    // 캐시 블록 할당 되었으므로 0으로 변경
  cache.cache_blocks[index].eviction_priority = CACHE_SIZE;  // 가장 최근 캐싱 되었으므로, 가장 큰 값 부여
  update_cache_eviction_priority(index);                     // 기존 나머지 캐시 블록들의 eviction_priority 값을 낮추어서 eviction 우선 순위를 높임

  V(&cache.cache_blocks[index].wmutex); // cache block 쓰기 lock 반환
}