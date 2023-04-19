#include <stdio.h>
#include "csapp.h"

#define WEBSERVER_HOST "localhost"
#define WEBSERVER_PORT 8080

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_SIZE 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *request_line_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";

static const char *host_header = "Host";
static const char *connection_header = "Connection";
static const char *proxy_connection_header = "Proxy-Connection";
static const char *user_agent_header = "User-Agent";

void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_webserver(char *hostname, int port, char *http_header);
void *thread_function(void *arg);

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
  int eviction_priority;      // LRU 알고리즘에 의한 소거 우선순위. 숫자가 작을수록 소거에 대한 우선 순위가 높아짐
  int is_empty; // 이 블럭에 캐시 정보가 들었는지 empty인지 아닌지 체크

  int reader_count;   // reader 수
  sem_t wmutex;     // cache block 쓰기 lock 여부
  sem_t rdcntmutex; // cache block 읽기 lock 여부
} cache_block;

typedef struct
{
  cache_block cache_blocks[CACHE_SIZE];
} Cache;

Cache cache;

int main(int argc, char **argv)
{
  int listenfd, *connfd_ptr;
  socklen_t clientlen;
  char hostname[MAXLINE], port[MAXLINE];
  pthread_t tid;
  struct sockaddr_storage clientaddr;

  cache_init();

  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port> \n", argv[0]);
    exit(1);
  }
  // 프로세스가 닫히거나 끊어진 파이프에 쓰기 요청을 할 경우 발생하는 오류(SIGPIPE)를 무시하고 서버를 계속 동작시킬 수 있음
  Signal(SIGPIPE, SIG_IGN); 

  listenfd = Open_listenfd(argv[1]);

  int connfd;

  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfd_ptr = (int *)malloc(sizeof(int));
    *connfd_ptr = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s %s).\n", hostname, port);

    pthread_create(&tid, NULL, thread_function, connfd_ptr);
  }
  return 0;
}

void *thread_function(void *arg)
{
  pthread_detach(pthread_self());

  int connfd = *((int *)arg);
  free(arg);
  doit(connfd);
  Close(connfd);
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
  sscanf(buf, "%s %s %s", method, uri, version); // read the client reqeust line

  if (strcasecmp(method, "GET"))
  {
    printf("Proxy does not implement the method");
    return;
  }

  char uri_copy[100];    
  strcpy(uri_copy, uri); 

  /* 요청 uri 주소가 캐싱되어 있는 주소 인지 */
  int cache_index;
  if ((cache_index = cache_find(uri_copy)) != -1)
  {
    get_cache_lock(cache_index); // 캐시 뮤텍스를 풀어줌 (열어줌 0->1)
    // 캐시에서 찾은 값을 connfd에 쓰고, 캐시에서 그 값을 바로 보내게 됨
    Rio_writen(connfd, cache.cache_blocks[cache_index].cache_obj, strlen(cache.cache_blocks[cache_index].cache_obj));
    put_cache_lock(cache_index); // 닫아줌 1->0 doit 끝
    return;
  }

  parse_uri(uri, hostname, path, &port); // uri 로부터 hostname, path, port 파싱하여 변수에 할당
  build_http_header(webserver_http_header, hostname, path, port, &rio); // hostname, path, port와 클라이언트 요청을 기반으로 웹 서버에 전송할 요청 헤더 재구성

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

void build_http_header(char *http_header, char *hostname, char *path, int port,
                       rio_t *client_rio)
{
  char buf[MAXLINE], request_line[MAXLINE], other_hdr[MAXLINE],
      host_hdr[MAXLINE];
  /* request line 생성 */
  sprintf(request_line, request_line_hdr_format, path);

  /* 클라이언트 입력 스트림 버퍼를 한 줄씩 읽어서 HTTP header를 만듦 */
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0)
  {
    if (strcmp(buf, endof_hdr) == 0)
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
          prox_hdr, user_agent_hdr, other_hdr, endof_hdr);
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
    cache.cache_blocks[i].eviction_priority = 0;      // 아직 캐싱된 데이터 없으므로 0, 최근에 할당 된 cache block 일 수록 높은 값을 가짐
    cache.cache_blocks[i].is_empty = 1; // 아직 캐싱된 데이터 없으므로 1

    // 두번째 파라미터 1이면 process shared, 0이면 thread shared, 세번째 파라미터 -> 세마포어 초기값 1(액세스 가능)
    Sem_init(&cache.cache_blocks[i].wmutex, 0, 1);     // -> 진입 가능한 자원 1개뿐이므로 binary semaphore
    Sem_init(&cache.cache_blocks[i].rdcntmutex, 0, 1); // -> 진입 가능한 자원 1개뿐이므로 binary semaphore
    cache.cache_blocks[i].reader_count = 0;
  }
}

/* cache block 접근 전 cache block access lock을 얻기 위한 함수 */
void get_cache_lock(int index)
{
  P(&cache.cache_blocks[index].rdcntmutex);      // reader count 값 변경에 대한 lock 획득

  cache.cache_blocks[index].reader_count++;        // read count 증가(조회 하러 들어가므로)
  if (cache.cache_blocks[index].reader_count == 1) // reader_count == 1 -> 현재 읽는 사용자 한명만 캐시 블록에 접근 중
    P(&cache.cache_blocks[index].wmutex);        // cache block에 대한 write lock 획득

  V(&cache.cache_blocks[index].rdcntmutex);      // reader count 값 변경에 대한 lock 반환
}

/* cache block 접근 끝난 이후 cache block access lock을 반환하기 위한 함수 */
void put_cache_lock(int index)
{
  P(&cache.cache_blocks[index].rdcntmutex);      // reader count 값 변경에 대한 lock 획득

  cache.cache_blocks[index].reader_count--;        // read count 감소(조회 끝났으므로)
  if (cache.cache_blocks[index].reader_count == 0) // reader_count == 0 -> 현재 읽는 사용자 한명만 캐시 블록에 접근 중
    V(&cache.cache_blocks[index].wmutex);        // cache block에 대한 write lock 획득

  V(&cache.cache_blocks[index].rdcntmutex);      // reader count 값 변경에 대한 lock 획득
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
      minindex = i;                 // i로 minindex 갱신
      min = cache.cache_blocks[i].eviction_priority; // min은 i번째 cache block의 eviction_priority 값으로 갱신
    }
    put_cache_lock(i);
  }
  return minindex;
}

void cache_eviction_priority(int index)
{
  for (int i = 0; i < CACHE_SIZE; i++)
  {
    if (i == index)
      continue;

    P(&cache.cache_blocks[i].wmutex); // cache block 쓰기 lock 획득

    if (cache.cache_blocks[i].is_empty == 0)
      cache.cache_blocks[i].eviction_priority--;    // 최근 캐싱된 cache block을 제외한 나머지 cache block eviction_priority 값을 감소 시킴

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
  cache.cache_blocks[index].eviction_priority = CACHE_SIZE;                // 가장 최근 캐싱 되었으므로, 가장 큰 값 부여
  cache_eviction_priority(index);                                       // 기존 나머지 캐시 블록들의 eviction_priority 값을 낮추어서 eviction 우선 순위를 높임

  V(&cache.cache_blocks[index].wmutex); // cache block 쓰기 lock 반환
}