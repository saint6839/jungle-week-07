/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
/* $begin adder */
#include "csapp.h"

int main(void) {
  char *buf, *p;
  char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
  int n1=0, n2=0;
  
  /* Extract two arguments */
  if ((buf = getenv("QUERY_STRING")) != NULL) { // Query string 값 찾아오기
    p = strchr(buf, '&'); // &가 처음 발견된 위치의 포인터 주소 반환, 즉 첫 번째 인자의 끝 위치를 찾을 수 있음
    *p = '\0'; // 첫 번째 인자의 끝을 나타내기 위해, & 문자를 null(\0)로 변경함.
    strcpy(arg1, buf); // buf의 첫번째 인자를 arg1에 복사
    strcpy(arg2, p+1); // p+1 위치에서의 인자를 arg2에 복사
    n1 = atoi(arg1); // arg1의 값을 정수로 변환하여 저장
    n2 = atoi(arg2); // arg2의 값을 정수로 변환하여 저장
  }

  /* Make the response body */
  sprintf(content, "QUERY_STRING=%s", buf);
  sprintf(content, "Welcome to add.com: ");
  sprintf(content, "%sTHE Internet addition portal.\r\n<p>", content);
  sprintf(content, "%sThe answer is: %d + %d = %d\r\n<p>", content, n1, n2, n1 + n2);
  sprintf(content, "%sThanks for visiting!\r\n", content);

  /* Generate the HTTP response */
  printf("Connection: close\r\n");
  printf("Content-length: %d\r\n", (int)strlen(content));
  printf("Content-type: text/html\r\n\r\n");
  printf("%s", content);
  fflush(stdout);

  exit(0);
}
/* $end adder */
