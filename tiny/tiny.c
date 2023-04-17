/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, char *method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
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

  listenfd = Open_listenfd(argv[1]);
  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfd_ptr = (int *)malloc(sizeof(int));
    *connfd_ptr = Accept(listenfd, (SA *)&clientaddr, &clientlen);

    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);

    printf("Accepted connection from (%s, %s)\n", hostname, port);
    pthread_create(&tid, NULL, thread_function, connfd_ptr);
    pthread_detach(tid); // 스레드 분리
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
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

  /* Read request line and headers */
  Rio_readinitb(&rio, connfd);
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers: \n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);

  // 동일할 경우 0 return
  // GET이나 HEAD 둘 중 하나라도 일치한다면 false이기 때문에, pass
  // 둘 다 해당되지 않을 경우 501 return
  if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD"))
  {
    clienterror(connfd, method, "501", "Not implemented", "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio);

  /* Parse URI from GET request */
  is_static = parse_uri(uri, filename, cgiargs);
  printf("파일 이름 :%s \n", filename);
  if (stat(filename, &sbuf) < 0)
  {
    clienterror(connfd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  /* Serve static content */
  if (is_static)
  {
    // 정적 컨텐츠일때 파일 형식이 일반 파일이고, 사용자에게 읽기 권한이 없다면
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
    {
      clienterror(connfd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    serve_static(connfd, filename, sbuf.st_size, method);
  }
  else
  {
    // 동적 컨텐츠일때 파일 형식이 일반 파일이고, 사용자에게 실행 권한이 없다면
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode))
    {
      clienterror(connfd, filename, "403", "Forbidden", "Tiny couldn't run CGI program");
      return;
    }
    serve_dynamic(connfd, filename, cgiargs);
  }
}

void clienterror(int connfd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body */
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body,
          "%s<body bgcolor="
          "ffffff"
          ">\r\n",
          body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(connfd, buf,
             strlen(buf)); // Rio_written() 한번 호출시마다 버퍼 비워짐
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(connfd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(connfd, buf, strlen(buf));
  Rio_writen(connfd, body, strlen(body));

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

int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  if (!strstr(uri, "cgi-bin"))
  { // 만약 동적이 아닌 정적 컨텐츠에 대한 요청이라면
    strcpy(cgiargs, "");
    strcpy(filename, "."); // .
    strcat(filename, uri); // .{uri}

    // uri의 끝이 / 라면, filename에 home.html을 이어 붙임
    if (uri[strlen(uri) - 1] == '/')
      strcat(filename, "home.html"); // .{uri}/home.html
    return 1;
  }
  else
  { // 동적 컨텐츠에 대한 요청이라면
    ptr = index(uri, '?');
    if (ptr)
    {
      strcpy(cgiargs, ptr + 1); // ? 뒤의 쿼리 파라미터 인자를 cgiargs에 복사함
      *ptr = '\0';
    }
    else
    {
      strcpy(cgiargs, "");
    }
    strcpy(filename, ".");
    strcat(filename, uri);
    return 0;
  }
}

void serve_dynamic(int connfd, char *filename, char *cgiargs)
{
  char buf[MAXLINE], *emptylist[] = {NULL};

  /* Return first part of HTTP response */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(connfd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(connfd, buf, strlen(buf)); // 클라이언트에 성공을 먼저 알린다

  /* 자식 프로세스 생성 성공 시*/
  if (Fork() == 0)
  {
    /* 부모 서버는 모든 CGI 변수들을 이 프로세스에 세팅한다.*/
    setenv("QUERY_STRING", cgiargs, 1);
    Dup2(connfd, STDOUT_FILENO);          // stdout을 클라이언트에게 redirect 시킨다.
    Execve(filename, emptylist, environ); // 현재 프로세스의 이미지를 filename의 이미지로 덮어씌우고 해당 파일 실행
  }
  Wait(NULL); /* 부모는 자식의 종료를 기다리기 위해, block 된 상태가 된다 */
}

void serve_static(int connfd, char *filename, int filesize, char *method)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Send response headers to client */
  get_filetype(filename, filetype);
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(connfd, buf, strlen(buf));
  printf("Response headers:\n");
  printf("%s", buf);

  /* Send response body to client */
  /* Http method HEAD일 경우에는 body 제외 */
  if (!strstr(method, "HEAD"))
  {
    srcfd = Open(filename, O_RDONLY, 0);

    srcp = (char *)malloc(filesize);  // heap 메모리에 공간 확보
    Rio_readn(srcfd, srcp, filesize); // 확보한 heap 공간에 srcfd에서 filesize 만큼을 srcp로 읽어온다.
    Close(srcfd);
    Rio_writen(connfd, srcp, filesize); // 클라이언트와 연결된 연결 식별자(fd)에 주소 srcp부터 filesize byte 만큼의 내용을 복사 -> // 클라이언트에게 응답 전달
    free(srcp);
  }
}

/*
 * get_filetype - 파일 이름으로부터 타입 추출
 */
void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".mp4"))
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
}