#ifndef MIME_H
#define MIME_H

/*
 * 경로의 마지막 '/' 뒤 이름에서 마지막 '.' 뒤를 확장자로 보고 D-29 표로 바꾼다(대소문자 무시, D-35).
 * 표에 없거나 확장자가 없으면 "application/octet-stream". 항상 NULL이 아닌 정적 문자열.
 */
const char *mime_type_for(const char *path);

#endif /* MIME_H */
