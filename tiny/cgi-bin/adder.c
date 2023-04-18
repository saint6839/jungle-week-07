/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
/* $begin adder */
#include "csapp.h"

int main(void)
{
  char *buf, *p;
  char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
  int n1 = 0, n2 = 0;

  /* Extract two arguments */
  if ((buf = getenv("QUERY_STRING")) != NULL)
  {                           // Query string 값 찾아오기
    char *token;              // 토큰을 저장할 변수
    token = strtok(buf, "&"); // &를 기준으로 문자열을 토큰으로 나눔

    while (token != NULL)
    {
      char key[256], value[256];             // 키와 값이 저장될 변수
      sscanf(token, "%[^=]=%s", key, value); // 토큰에서 키와 값을 추출

      if (strcmp(key, "first") == 0)
      {                      // 키가 "first"인 경우
        strcpy(arg1, value); // arg1에 값을 복사
        n1 = atoi(arg1);     // arg1의 값을 정수로 변환하여 저장
      }
      else if (strcmp(key, "second") == 0)
      {                      // 키가 "second"인 경우
        strcpy(arg2, value); // arg2에 값을 복사
        n2 = atoi(arg2);     // arg2의 값을 정수로 변환하여 저장
      }
      token = strtok(NULL, "&"); // 다음 토큰으로 이동
    }
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
  fflush(stdout); // stdout의 버퍼를 비우면서, 클라이언트에게 전달한다.

  exit(0);
}
/* $end adder */
