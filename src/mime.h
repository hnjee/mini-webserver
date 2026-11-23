#ifndef MIME_H
#define MIME_H

/*
 * 파일 경로의 확장자로 Content-Type을 돌려준다.
 * 확장자는 마지막 '/' 뒤 이름에서 마지막 '.' 뒤이고, 대소문자를 구분하지 않는다.
 * 모르는 확장자나 확장자가 없으면 "application/octet-stream"이다.
 */
const char *mime_type(const char *path);

#endif
