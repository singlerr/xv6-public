#include "types.h"
#include "x86.h"

void *
memset(void *dst, int c, uint n)
{
  if ((int)dst % 4 == 0 && n % 4 == 0)
  {
    c &= 0xFF;
    stosl(dst, (c << 24) | (c << 16) | (c << 8) | c, n / 4);
  }
  else
    stosb(dst, c, n);
  return dst;
}

int memcmp(const void *v1, const void *v2, uint n)
{
  const uchar *s1, *s2;

  s1 = v1;
  s2 = v2;
  while (n-- > 0)
  {
    if (*s1 != *s2)
      return *s1 - *s2;
    s1++, s2++;
  }

  return 0;
}

void *
memmove(void *dst, const void *src, uint n)
{
  const char *s;
  char *d;

  s = src;
  d = dst;
  if (s < d && s + n > d)
  {
    s += n;
    d += n;
    while (n-- > 0)
      *--d = *--s;
  }
  else
    while (n-- > 0)
      *d++ = *s++;

  return dst;
}

// memcpy exists to placate GCC.  Use memmove.
void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

int strncmp(const char *p, const char *q, uint n)
{
  while (n > 0 && *p && *p == *q)
    n--, p++, q++;
  if (n == 0)
    return 0;
  return (uchar)*p - (uchar)*q;
}

char *
strncpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  while (n-- > 0 && (*s++ = *t++) != 0)
    ;
  while (n-- > 0)
    *s++ = 0;
  return os;
}

// Like strncpy but guaranteed to NUL-terminate.
char *
safestrcpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  if (n <= 0)
    return os;
  while (--n > 0 && (*s++ = *t++) != 0)
    ;
  *s = 0;
  return os;
}

int strlen(const char *s)
{
  int n;

  for (n = 0; s[n]; n++)
    ;
  return n;
}

void reverse(char str[], int length)
{
  int start = 0;
  int end = length - 1;
  while (start < end)
  {
    char temp = str[start];
    str[start] = str[end];
    str[end] = temp;
    start++;
    end--;
  }
}

char *itoa(int num, int base, char *str)
{
  int i = 0;
  int isNegative = 0;

  if (num == 0)
  {
    str[i++] = '0';
    str[i] = '\0';
    return str;
  }
  if (num < 0 && base == 10)
  {
    isNegative = 1;
    num = num < 0 ? -num : num;
  }

  while (num != 0)
  {
    int rem = num % base;
    str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
    num = num / base;
  }

  if (isNegative)
  {
    str[i++] = '-';
  }

  str[i] = '\0';
  reverse(str, i);

  return str;
}

int katoi(const char *s)
{
  int n;

  n = 0;
  while ('0' <= *s && *s <= '9')
    n = n * 10 + *s++ - '0';
  return n;
}