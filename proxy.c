#include <stdio.h>
#include "csapp.h"

// Proxy part.3 - Cache
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define LRU_MAGIC_NUMBER 9999
// Least Recently Used
// LRU: 가장 오랫동안 참조되지 않은 페이지를 교체하는 기법

#define CACHE_OBJS_COUNT 10

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

void *thread(void *vargsp);
void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_webserver(char *hostname, int port, char *http_header);
void *thread_function(void *arg);

// cache function
void cache_init();
int cache_find(char *url);
void cache_uri(char *uri, char *buf);

void reader_pre(int i);
void reader_after(int i);

typedef struct
{
  char cache_obj[MAX_OBJECT_SIZE];
  char cache_url[MAXLINE];
  int LRU;     // least recently used 가장 최근에 사용한 것의 우선순위를 뒤로 미움 (캐시에서 삭제할 때)
  int is_empty; // 이 블럭에 캐시 정보가 들었는지 empty인지 아닌지 체크

  int read_count;      // count of readers
  sem_t wmutex;     // protects accesses to cache 세마포어 타입. 1: 사용가능, 0: 사용 불가능
  sem_t rdcntmutex; // protects accesses to read_count
} cache_block;      // 캐쉬블럭 구조체로 선언

typedef struct
{
  cache_block cacheobjs[CACHE_OBJS_COUNT]; // ten cache blocks
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
  Signal(SIGPIPE, SIG_IGN);

  listenfd = Open_listenfd(argv[1]);
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
  int web_connfd;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char webserver_http_header[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE];
  int port;

  // rio: client's rio / server_rio: webserver's rio
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

  char url_store[100];    // 아직 doit 함수 ㅎㅎ
  strcpy(url_store, uri); // doit으로 받아온 connfd가 들고있는 uri를 넣어준다

  /* 요청 url 주소가 캐싱되어 있는 주소 인지 */
  int cache_index;
  if ((cache_index = cache_find(url_store)) != -1)
  {                         
    reader_pre(cache_index); // 캐시 뮤텍스를 풀어줌 (열어줌 0->1)
    Rio_writen(connfd, cache.cacheobjs[cache_index].cache_obj, strlen(cache.cacheobjs[cache_index].cache_obj));
    // 캐시에서 찾은 값을 connfd에 쓰고, 캐시에서 그 값을 바로 보내게 됨
    reader_after(cache_index); // 닫아줌 1->0 doit 끝
    return;
  }

  // parse the uri to get hostname, file path, port
  parse_uri(uri, hostname, path, &port);

  // build the http header which will send to the end server
  build_http_header(webserver_http_header, hostname, path, port, &rio);

  // connect to the end server
  web_connfd = connect_webserver(hostname, port, webserver_http_header);
  if (web_connfd < 0)
  {
    printf("connection failed\n");
    return;
  }

  Rio_readinitb(&server_rio, web_connfd);

  // write the http header to webserver
  Rio_writen(web_connfd, webserver_http_header, strlen(webserver_http_header));

  // recieve message from end server and send to the client
  char cachebuf[MAX_OBJECT_SIZE];
  int sizebuf = 0;
  size_t n; // 캐시에 없을 때 찾아주는 과정?
  while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
  {
    // printf("proxy received %ld bytes, then send\n", n);
    sizebuf += n;
    /* proxy거쳐서 서버에서 response오는데, 그 응답을 저장하고 클라이언트에 보냄 */
    if (sizebuf < MAX_OBJECT_SIZE) // 작으면 response 내용을 적어놈
      strcat(cachebuf, buf);       // cachebuf에 but(response값) 다 이어붙혀놓음(캐시내용)
    Rio_writen(connfd, buf, n);
  }
  Close(web_connfd);

  // store it
  if (sizebuf < MAX_OBJECT_SIZE)
  {
    cache_uri(url_store, cachebuf); // url_store에 cachebuf 저장
  }
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
      break; /*EOF*/

    /* 대소문자 여부 상관 없이 비교 if true -> return 0 */
    if (!strncasecmp(buf, host_header, strlen(host_header)))
    {
      strcpy(host_hdr, buf);
      continue;
    }

    /* 기타 헤더 정보 */
    if (!strncasecmp(buf, connection_header, strlen(connection_header)) &&
        !strncasecmp(buf, proxy_connection_header,
                     strlen(proxy_connection_header)) &&
        !strncasecmp(buf, user_agent_header, strlen(user_agent_header)))
    {
      strcat(other_hdr, buf);
    }
  }
  if (strlen(host_hdr) == 0)
  {
    sprintf(host_hdr, host_hdr_format, hostname);
  }
  sprintf(http_header, "%s%s%s%s%s%s%s", request_line, host_hdr, conn_hdr,
          prox_hdr, user_agent_hdr, other_hdr, endof_hdr);
  printf("%s\n", http_header);

  return;
}
// Connect to the end server
int connect_webserver(char *hostname, int port, char *http_header)
{
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr);
}

/* 요청된 uri로부터 hostname, path, port를 parsing */
void parse_uri(char *uri, char *hostname, char *path, int *port)
{
  /* default webserver host, port */
  strcpy(hostname, "localhost");
  *port = 8080;

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

void cache_init()
{
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    cache.cacheobjs[i].LRU = 0;     // LRU : 우선 순위를 미는 것. 처음이니까 0
    cache.cacheobjs[i].is_empty = 1; // 1이 비어있다는 뜻

    // Sem_init : 세마포어 함수
    // 첫 번째 인자: 초기화할 세마포어의 포인터 / 두 번째: 0 - 쓰레드들끼리 세마포어 공유, 그 외 - 프로세스 간 공유 / 세 번째: 초기 값
    //    뮤텍스 만들 포인터 / 0 : 세마포어를 뮤텍스로 쓰려면 0을 써야 쓰레드끼리 사용하는거라고 표시하는 것이 됨 / 1 : 초깃값
    // 세마포어는 프로세스를 쓰는 것. 지금 세마포어를 쓰레드에 적용하고 싶으니까 0을 써서 쓰레드에서 쓰는거라고 표시, 나머지 숫자를 프로세스에서 쓰는거라는 표시.
    Sem_init(&cache.cacheobjs[i].wmutex, 0, 1);     // wmutex : 캐시에 접근하는 것을 프로텍트해주는 뮤텍스
    Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1); // read count mutex : 리드카운트에 접근하는걸 프로텍트해주는 뮤텍스
    // ㄴ flag 지정
    cache.cacheobjs[i].read_count = 0; // read count를 0으로 놓고 init을 끝냄
  }
}

void reader_pre(int i)
{ // i = 해당인덱스
  // 내가 받아온 index오브젝트의 리드카운트 뮤텍스를 P함수(recntmutex에 접근을 가능하게) 해준다
  /* rdcntmutex로 특정 read_count에 접근하고 +1해줌. 원래 0으로 세팅되어있어서, 누가 안쓰고 있으면 0이었다가 1로 되고 if문 들어감 */
  P(&cache.cacheobjs[i].rdcntmutex); // P연산(locking):정상인지 검사, 기다림 (P함수 비정상이면 에러 도출되는 로직임)
  cache.cacheobjs[i].read_count++;      // read_count 풀고 들어감
  /* 조건문 들어오면 그때서야 캐쉬에 접근 가능. 그래서 만약 누가 쓰고있어도 P, read_count까지는 할 수 있는데 +1이 되니까 1->2가 되고
    그러면 캐시에 접근을 못하게 됨. but reader_after에서 -1 다시 내려주기때문에 0, 1, 0 에서만 움직임 */
  if (cache.cacheobjs[i].read_count == 1)
    P(&cache.cacheobjs[i].wmutex);   // write mutex 뮤텍스를 풀고(캐시에 접근)
  V(&cache.cacheobjs[i].rdcntmutex); // V연산 풀기(캐시 쫒아냄) / read count mutex
}

void reader_after(int i)
{
  P(&cache.cacheobjs[i].rdcntmutex);
  cache.cacheobjs[i].read_count--;
  if (cache.cacheobjs[i].read_count == 0)
    V(&cache.cacheobjs[i].wmutex);
  V(&cache.cacheobjs[i].rdcntmutex);
}

/* feedback : if문 중간에 멈출 필요 없음 */
int cache_find(char *url)
{
  printf("%s", url);
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    reader_pre(i);
    if (cache.cacheobjs[i].is_empty == 0 && strcmp(url, cache.cacheobjs[i].cache_url) == 0)
    {
      reader_after(i);
      return i;
    }
    reader_after(i);
  }
  return -1;
}

int cache_eviction()
{ // 캐시 쫒아내기
  int min = LRU_MAGIC_NUMBER;
  int minindex = 0;
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    reader_pre(i);
    if (cache.cacheobjs[i].is_empty == 1)
    {
      minindex = i;
      reader_after(i);
      break;
    }
    if (cache.cacheobjs[i].LRU < min)
    {
      minindex = i;
      min = cache.cacheobjs[i].LRU;
      reader_after(i);
      continue;
    }
    reader_after(i);
  }
  return minindex;
}

void writePre(int i)
{
  P(&cache.cacheobjs[i].wmutex);
}

void writeAfter(int i)
{
  V(&cache.cacheobjs[i].wmutex);
}

/* feedback : index 반으로 나눌 필요 없음 */
void cache_LRU(int index)
{
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    if (i == index)
    {
      continue;
    }
    writePre(i);
    if (cache.cacheobjs[i].is_empty == 0)
    {
      cache.cacheobjs[i].LRU--;
    }
    writeAfter(i);
  }
}

// cache the uri and content in cache
void cache_uri(char *uri, char *buf)
{
  int i = cache_eviction(); // 빈 캐시 블럭을 찾는 첫번째 index

  writePre(i);

  strcpy(cache.cacheobjs[i].cache_obj, buf);
  strcpy(cache.cacheobjs[i].cache_url, uri);
  cache.cacheobjs[i].is_empty = 0;
  cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER; // 가장 최근에 했으니 우선순위 9999로 보내줌
  cache_LRU(i);                              // 나 빼고 LRU 다 내려.. 난 9999니까

  writeAfter(i);
}