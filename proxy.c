#include <stdio.h>

#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *request_line_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

static const char *connection_header = "Connection";
static const char *user_agent_header = "User-Agent";
static const char *proxy_connection_header = "Proxy-Connection";
static const char *host_header = "Host";

void doit(int fd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void read_requesthdrs(rio_t *rp);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_webserver(char *hostname, int port);
void thread_function(void *arg);

int main(int argc, char **argv)
{
  int listenfd, *connfd_ptr;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  pthread_t tid;

  /* Check command line args */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  Signal(SIGPIPE, SIG_IGN); // 비정상적으로 소켓 연결이 끊어질 경우, 프로세스가 종료되어버리는데 한 클라이언트에 대해서 소켓 연결이 끊어지더라도 전체 프로세스는 유지되어지도록 보장
  listenfd = Open_listenfd(argv[1]);
  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfd_ptr = (int *)malloc(sizeof(int));
    *connfd_ptr = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);

    printf("Accepted connection from (%s, %s)\n", hostname, port);
    pthread_create(&tid, NULL, thread_function, connfd_ptr);
    pthread_detach(tid);
  }
}

void thread_function(void *arg)
{
  int connfd = *((int *)arg);
  free(arg);
  doit(connfd);
  Close(connfd);
  pthread_exit(NULL);
}

void doit(int connfd)
{
  int web_connfd;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char webserver_http_header[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE];
  int port;

  rio_t rio;
  rio_t server_rio;

  /* Read request line */
  Rio_readinitb(&rio, connfd);
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers: \n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);

  if (strcasecmp(method, "GET"))
  {
    printf("501 method not implemented");
    return;
  }
  parse_uri(uri, hostname, path, &port);                                // hostname, path, port 할당
  web_connfd = connect_webserver(hostname, port); // hostname과 port에 해당하는 file descriptor 생성
  if (web_connfd < 0)
  {
    printf("connection failed\n");
    return;
  }
  build_http_header(webserver_http_header, hostname, path, port, &rio); // 클라이언트의 요청 헤더를 웹 서버에 전달 하기 위한 헤더 구성
  Rio_readinitb(&server_rio, web_connfd); // web_connfd 입력 스트림으로 server_rio 초기화
  Rio_writen(web_connfd, webserver_http_header, strlen(webserver_http_header)); // 앞서 구성한 웹 서버 전달용 요청 헤더를 웹 서버와 연결된 file descriptor를 통해 작성(전송)

  /* web server로부터 받은 응답에 대한 입력스트림을 버퍼에서 한 줄씩 읽어 client에게 응답 반환*/
  size_t n;
  while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
  {
    // printf("proxy send to client: %s\n", buf); // 전송 테스트 
    Rio_writen(connfd, buf, n);
  }
  Close(web_connfd);
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

/* 요청된 uri로부터 hostname, path, port를 parsing */
void parse_uri(char *uri, char *hostname, char *path, int *port)
{
  *port = 8080; /* port 번호 입력 없더라도 8080 webserver로 redirecting */
  char *pos = strstr(uri, "//");
  pos = pos != NULL ? pos + 2 : uri;

  char *pos2 = strstr(pos, ":");

  // chrome, safari 등 외부 브라우저 접속시
  strcpy(hostname, "localhost");

  if (pos2 != NULL)
  {
    *pos2 = '\0';
    sscanf(pos, "%s", hostname);
    sscanf(pos2 + 1, "%d%s", port, path);
  }
  else
  {
    pos2 = strstr(pos, "/");
    if (pos2 != NULL)
    {
      *pos2 = '\0';

      sscanf(pos, "%s", hostname);
      *pos2 = '/';
      sscanf(pos2, "%s", path);
    }
    else
    {
      sscanf(pos, "%s", hostname);
    }
  }
  return;
}

void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  while (strcmp(buf, "\r\n"))
  {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

/* 프록시 서버와 웹 서버 연결 */
int connect_webserver(char *hostname, int port)
{
  char port_str[100];
  sprintf(port_str, "%d", port);
  return Open_clientfd(hostname, port_str);
}