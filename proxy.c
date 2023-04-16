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
static const char *requestlint_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

static const char *connection_header = "Connection";
static const char *user_agent_header = "User-Agent";
static const char *proxy_connection_header = "Proxy-Connection";
static const char *host_header = "Host";

void doit(int fd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void read_requesthdrs(rio_t *rp);
void build_http_header(char *http_header, char *hostname, char *path, int port,
                       rio_t *client_rio);
int connect_webserver(char *hostname, int port);
void thread_function(void *arg);

int main(int argc, char **argv) {
  int listenfd, *connfd_ptr;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  pthread_t tid;

  /* Check command line args */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  listenfd = Open_listenfd(argv[1]);
  while (1) {
    clientlen = sizeof(clientaddr);
    connfd_ptr = (int *)malloc(sizeof(int));
    *connfd_ptr = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);

    printf("Accepted connection from (%s, %s)\n", hostname, port);
    pthread_create(&tid, NULL, thread_function, connfd_ptr);
    pthread_detach(tid);
  }
}

void thread_function(void *arg) {
  int connfd = *((int *)arg);
  free(arg);
  doit(connfd);
  Close(connfd);
  pthread_exit(NULL);
}

void doit(int connfd) {
  int webserverfd;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char webserver_http_header[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE];
  int port;

  rio_t rio;
  rio_t server_rio;

  /* Read request line and headers */
  Rio_readinitb(&rio, connfd);
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers: \n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);

  if (strcasecmp(method, "GET")) {
    printf("프록시 서버에서 지원하지 않는 HTTP 메서드 입니다.");
    return;
  }
  parse_uri(uri, hostname, path, &port);

  build_http_header(webserver_http_header, hostname, path, port, &rio);
  if (strcmp(hostname, ",")) {
    strcpy(hostname, "localhost");
  }
  webserverfd = connect_webserver(hostname, port);
  if (webserverfd < 0) {
    printf("연결 실패\n");
    return;
  }

  Rio_readinitb(&server_rio, webserverfd);
  /*write the http header to webserver*/
  Rio_writen(webserverfd, webserver_http_header, strlen(webserver_http_header));

  /*receive message from end server and send to the client*/
  size_t n;
  while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0) {
    printf("proxy send to client: %s\n", buf);
    Rio_writen(connfd, buf, n);
  }
  Close(webserverfd);
}

void build_http_header(char *http_header, char *hostname, char *path, int port,
                       rio_t *client_rio) {
  char buf[MAXLINE], request_header[MAXLINE], other_hdr[MAXLINE],
      host_hdr[MAXLINE];
  /*request line*/
  sprintf(request_header, requestlint_hdr_format, path);
  /*get other request header for client rio and change it */
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
    if (strcmp(buf, endof_hdr) == 0) break; /*EOF*/

    if (!strncasecmp(buf, host_header, strlen(host_header))) /*Host:*/
    {
      strcpy(host_hdr, buf);
      continue;
    }

    /* other header informations  */
    if (!strncasecmp(buf, connection_header, strlen(connection_header)) &&
        !strncasecmp(buf, proxy_connection_header,
                     strlen(proxy_connection_header)) &&
        !strncasecmp(buf, user_agent_header, strlen(user_agent_header))) {
      strcat(other_hdr, buf);
    }
  }
  if (strlen(host_hdr) == 0) {
    sprintf(host_hdr, host_hdr_format, hostname);
  }
  sprintf(http_header, "%s%s%s%s%s%s%s", request_header, host_hdr, conn_hdr,
          prox_hdr, user_agent_hdr, other_hdr, endof_hdr);
  printf("%s\n", http_header);

  return;
}

void parse_uri(char *uri, char *hostname, char *path, int *port) {
  *port = 8080;
  char *pos = strstr(uri, "//");
  pos = pos != NULL ? pos + 2 : uri;

  char *pos2 = strstr(pos, ":");
  if (pos2 != NULL) {
    *pos2 = '\0';
    sscanf(pos, "%s", hostname);
    sscanf(pos2 + 1, "%d%s", port, path);

  } else {
    pos2 = strstr(pos, "/");
    if (pos2 != NULL) {
      *pos2 = '\0';

      sscanf(pos, "%s", hostname);
      *pos2 = '/';
      sscanf(pos2, "%s", path);
    } else {
      sscanf(pos, "%s", hostname);
    }
  }
  return;
}

void read_requesthdrs(rio_t *rp) {
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  while (strcmp(buf, "\r\n")) {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

/*Connect to the end server*/
int connect_webserver(char *hostname, int port) {
  char port_str[100];
  sprintf(port_str, "%d", port);
  return Open_clientfd(hostname, port_str);
}
